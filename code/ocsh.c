#define _POSIX_C_SOURCE 200809L /* active les fonctions POSIX : sigaction, getline, setenv... */
#include <ctype.h>    /* isspace, isalnum, isalpha */
#include <dirent.h>   /* opendir, readdir : lister un dossier (complétion) */
#include <errno.h>    /* errno, EINTR, ENOENT */
#include <fcntl.h>    /* open, O_RDONLY, O_WRONLY, O_CREAT... */
#include <signal.h>   /* sigaction, sigprocmask, SIGINT, SIGCHLD */
#include <stdio.h>    /* printf, fprintf, fopen... */
#include <stdlib.h>   /* malloc, free, exit, getenv, setenv */
#include <string.h>   /* strlen, strcmp, strdup, memcpy... */
#include <sys/stat.h> /* stat, S_ISDIR */
#include <sys/types.h>/* pid_t */
#include <sys/wait.h> /* waitpid, WIFEXITED, WEXITSTATUS... */
#include <termios.h>  /* mode du terminal (lecture touche par touche) */
#include <unistd.h>   /* fork, execvp, pipe, dup2, chdir, getcwd, read, write */

#define MAX_ARGS 128   /* nombre max de mots dans une commande */
#define MAX_REDIR 8    /* nombre max de redirections dans une commande */
#define MAX_PIPE 16    /* nombre max de commandes dans un pipeline a -> b -> c */
#define MAX_VARS 256   /* nombre max de variables du shell */
#define MAX_HIST 500   /* nombre max d'entrées d'historique */
#define MAX_LINE 1024  /* taille max d'une ligne tapée au clavier */
#define MAX_CAND 256   /* nombre max de propositions de complétion */
#define MAX_JOBS 32    /* nombre max de jobs (tâches de fond / arrêtées) suivis en même temps */

/* ---------------------
 * ÉTAT GLOBAL DU SHELL 
 * ---------------------*/
static int last_status = 0;      /* $? : code de retour de la dernière commande */
static pid_t last_bg = 0;        /* $! : PID de la dernière commande lancée avec & */
static int exit_requested = 0;   /* passe à 1 quand on tape "quit" */
static int exit_code = 0;        /* code de sortie du shell */
static int interactive = 0;      /* 1 si on lit au clavier (terminal) */
static sigset_t chld_set;        /* ensemble contenant seulement SIGCHLD (pour le bloquer) */
/* volatile sig_atomic_t : variables modifiées par un gestionnaire de signal */
static volatile sig_atomic_t interrupted = 0;  /* mis à 1 par Ctrl+C (reçu par le shell, ou subi par une commande) */

/* [BLOC] TABLE DES JOBS (job control). Un "job" = un pipeline lancé avec & ou
 * arrêté avec Ctrl+Z. Le shell garde la liste pour pouvoir l'afficher (jobs),
 * le ramener au premier plan (fg) ou le relancer (bg). */
#define JOB_RUNNING 0   /* le job tourne */
#define JOB_STOPPED 1   /* le job est en pause (Ctrl+Z) */
#define JOB_DONE    2   /* le job est terminé (à annoncer puis à oublier) */
typedef struct {
    int used;                         /* 1 si la case est occupée, 0 si libre */
    int id;                           /* numéro affiché : [1], [2]... */
    pid_t pgid;                       /* groupe de processus du job (= PID de sa 1re commande) */
    volatile pid_t pids[MAX_PIPE];    /* PID de chaque commande du pipeline (0 = déjà terminée) */
    int npids;                        /* nombre de commandes dans le pipeline */
    pid_t last_pid;                   /* PID de la dernière commande : son code de retour = celui du job */
    volatile sig_atomic_t state;      /* JOB_RUNNING / JOB_STOPPED / JOB_DONE (modifié par le gestionnaire SIGCHLD) */
    char cmd[128];                    /* texte de la commande (pour l'affichage) */
} Job;
static Job jobs[MAX_JOBS];

/* Petit type "Command" : ce que l'analyse d'une commande simple produit. */
typedef struct {
    int target;      /* descripteur visé : 0 (entrée) ou 1 (sortie) */
    int mode;        /* 0 = '<' lecture, 1 = '>' écrasement, 2 = '>>' ajout */
    char *file;      /* nom du fichier */
} Redir;
typedef struct {
    char *argv[MAX_ARGS + 1];  /* mots de la commande, terminés par NULL (pour execvp) */
    int argc;                  /* nombre de mots */
    Redir redirs[MAX_REDIR];   /* redirections trouvées */
    int nredir;                /* nombre de redirections */
} Command;

/* Tableau dynamique de "statements" (instructions) : utilisé par la partie 2. */
typedef struct {
    char **v;
    int n, cap;
} Stmts;

/* Buffer de texte qui grandit tout seul. */
typedef struct {
    char *s;           /* le texte (terminé par '\0') */
    size_t len, cap;   /* longueur utilisée / mémoire réservée */
} Buf;

/* ---- Prototypes : permettent à une fonction d'en appeler une définie plus bas ---- */
static int exec_statement(const char *stmt);          /* partie 1 */
static int run_source(const char *src);               /* partie 2 */
static char *expand(const char *in);                  /* partie 2 */
static void var_set(const char *name, const char *value, int exported);
static const char *var_get(const char *name);
static void var_export_words(Command *c);
static void history_print(void);
static int job_fg(Command *c);                        /* job control (section 1.F) */
static int job_bg(Command *c);
static void jobs_print(void);

/* ---------------------------------------------------------------------
 * OUTILS GÉNÉRAUX (mémoire et texte)
 * --------------------------------------------------------------------- */

/* Réserve de la mémoire ; si ça échoue on s'arrête (inutile de continuer). */
static void *xmalloc(size_t n)
{
    void *p = malloc(n);                       // demande n octets au système
    if (!p) { perror("ocsh: malloc"); exit(1); } // plus de mémoire : on quitte
    return p;
}
/* Agrandit un bloc mémoire (même logique que xmalloc). */
static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n);
    if (!p) { perror("ocsh: realloc"); exit(1); }
    return p;
}
/* Copie une chaîne dans un nouveau bloc mémoire (à libérer avec free). */
static char *xstrdup(const char *s)
{
    char *p = xmalloc(strlen(s) + 1);  // +1 pour le '\0' final
    strcpy(p, s);                      // copie la chaîne, '\0' compris
    return p;
}
/* Copie les n premiers caractères de s dans un nouveau bloc. */
static char *slice(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);   // copie n octets
    p[n] = '\0';       // on termine la chaîne nous-mêmes
    return p;
}
/* Ajoute n caractères à la fin d'un Buf (il s'agrandit si besoin). */
static void buf_addn(Buf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {             // pas assez de place ?
        b->cap = (b->len + n + 1) * 2;         // on double (moins de realloc)
        b->s = xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, s, n);               // copie à la fin
    b->len += n;                               // nouvelle longueur
    b->s[b->len] = '\0';                       // toujours terminé par '\0'
}
static void buf_add(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }
static void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }
/* Récupère le texte du Buf (le Buf devient vide). Jamais NULL. */
static char *buf_take(Buf *b)
{
    char *r = b->s ? b->s : xstrdup("");
    b->s = NULL;
    b->len = b->cap = 0;
    return r;
}
/* =====================================================================
 *                         PARTIE 1  (jalons 1, 2, 3)
 *   REPL de base, fork/execvp/wait, builtins + alias, redirections (dup2),
 *   pipes "->", arrière-plan "&", signaux SIGINT / SIGCHLD.
 * ===================================================================== */

/* ---------- 1.A  Table des alias et des builtins (jalon 2) ---------- */

/* ALIAS : un nom "ocsh" qui est traduit en vraie commande Linux.
 * Ce sont de VRAIS programmes : on les lance donc avec fork + execvp. */
static const char *aliases[][2] = {
    {"list", "ls"},   {"show", "cat"},   {"copy", "cp"},   {"move", "mv"},
    {"del", "rm"},    {"newdir", "mkdir"}, {"newfile", "touch"},
};
#define N_ALIASES (sizeof aliases / sizeof aliases[0])

/*  BUILTINS : commandes exécutées par le shell LUI-MÊME (pas de fork+exec).
 * "go" (cd) DOIT être un builtin : un chdir fait dans un enfant ne changerait
 * que le dossier de l'enfant, qui meurt aussitôt. Le dossier courant appartient
 * au processus ; pour changer celui du shell, c'est le shell qui doit chdir().
 * Même raison pour jobs / fg / bg : la table des jobs appartient au shell. */
static const char *builtins[] = {"go", "quit", "path", "say", "info", "past",
                                 "osinfo", "export", "jobs", "fg", "bg", NULL};

/* Vrai si name est un builtin. */
static int is_builtin(const char *name)
{
    for (int i = 0; builtins[i]; i++)       // parcourt la liste
        if (!strcmp(name, builtins[i]))     // strcmp == 0 : textes identiques
            return 1;
    return 0;
}
/* Traduit un alias en vraie commande ("list" -> "ls") ; sinon renvoie name. */
static const char *resolve_alias(const char *name)
{
    for (size_t i = 0; i < N_ALIASES; i++)
        if (!strcmp(name, aliases[i][0]))
            return aliases[i][1];
    return name;
}

/* ---------- 1.B  Analyse d'une commande simple (mots + redirections) ---------- */

/* Lit UN mot à partir de *pp, enlève les guillemets, et avance *pp.
 * Le mot s'arrête à un espace ou à < > (hors guillemets).
 *   'a b'  -> a b     "a b" -> a b     a\ b -> a b      */
static char *parse_word(const char **pp)
{
    const char *p = *pp;   // position de lecture
    Buf w = {0};           // le mot en construction
    char q = 0;            // guillemet ouvert : 0 aucun, ' ou "
    while (*p) {
        char c = *p;
        if (q == '\'') {                       // dans '...' : tout est littéral
            if (c == '\'') q = 0; else buf_addc(&w, c);
            p++;
        } else if (q == '"') {                 // dans "..." : seul \ est spécial
            if (c == '"') q = 0;
            else if (c == '\\' && p[1]) { p++; buf_addc(&w, *p); }  // \x -> x
            else buf_addc(&w, c);
            p++;
        } else {                               // hors guillemets
            if (isspace((unsigned char)c) || c == '<' || c == '>')
                break;                         // fin du mot
            if (c == '\'' || c == '"') q = c;  // ouvre un guillemet (non copié)
            else if (c == '\\' && p[1]) { p++; buf_addc(&w, *p); }
            else buf_addc(&w, c);
            p++;
        }
    }
    *pp = p;
    return buf_take(&w);
}

/* Transforme le texte d'UNE commande ("say hi > f.txt") en structure Command.
 * Renvoie 0 si OK, -1 si erreur de syntaxe (message déjà affiché). */
static int parse_command(const char *seg, Command *c)
{
    const char *p = seg;
    while (1) {
        while (isspace((unsigned char)*p)) p++;   // saute les espaces
        if (!*p) break;                           // fin du texte
        if (*p == '<' || *p == '>') {             // [BLOC] une redirection
            Redir r;
            if (*p == '<') { r.target = 0; r.mode = 0; p++; }              // <  : entrée
            else if (p[1] == '>') { r.target = 1; r.mode = 2; p += 2; }    // >> : ajout
            else { r.target = 1; r.mode = 1; p++; }                        // >  : écrase
            while (isspace((unsigned char)*p)) p++;   // espaces avant le nom du fichier
            if (!*p || *p == '<' || *p == '>') {      // pas de fichier après l'opérateur
                fprintf(stderr, "ocsh: erreur de syntaxe : nom de fichier attendu\n");
                return -1;
            }
            r.file = parse_word(&p);                  // lit le nom du fichier
            if (c->nredir >= MAX_REDIR) { fprintf(stderr, "ocsh: trop de redirections\n"); return -1; }
            c->redirs[c->nredir++] = r;
        } else {                                  // [BLOC] un mot normal
            if (c->argc >= MAX_ARGS) { fprintf(stderr, "ocsh: trop d'arguments\n"); return -1; }
            c->argv[c->argc++] = parse_word(&p);
        }
    }
    c->argv[c->argc] = NULL;   // execvp exige un tableau terminé par NULL
    return 0;
}

/* Libère la mémoire d'une Command. */
static void free_command(Command *c)
{
    for (int i = 0; i < c->argc; i++) free(c->argv[i]);
    for (int i = 0; i < c->nredir; i++) free(c->redirs[i].file);
}

/* Coupe le texte s sur chaque "->" (hors guillemets). On modifie s sur place :
 * chaque "->" est remplacé par '\0', et segs[i] pointe sur le début de la case i.
 * Renvoie le nombre de cases (ou -1 si trop de commandes). */
static int split_pipeline(char *s, char **segs, int max)
{
    int n = 0;
    char q = 0;
    segs[n++] = s;                                // la 1re case commence au début
    for (char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }  // dans des guillemets : on ignore
        if (*p == '\'' || *p == '"') { q = *p; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }// caractère échappé : on saute
        if (p[0] == '-' && p[1] == '>') {         // trouvé le symbole ->
            if (n >= max) return -1;              // trop de commandes
            *p = '\0';                            // termine la case précédente
            segs[n++] = p + 2;                    // la suivante commence après "->"
            p++;                                  // saute le '>'
        }
    }
    return n;
}

/* ---------- 1.C  Redirections avec dup2 (jalon 2) ---------- */

/*  Applique les redirections d'une commande.
 * IDÉE : les descripteurs 0 (stdin), 1 (stdout), 2 (stderr) sont des numéros.
 * open() donne un nouveau numéro fd ; dup2(fd, 1) fait pointer le numéro 1 vers
 * le même fichier. Après ça, tout ce que le programme écrit sur "stdout" va dans
 * le fichier. Le programme ne s'en rend pas compte. */
static int apply_redirs(Command *c)
{
    for (int i = 0; i < c->nredir; i++) {
        Redir *r = &c->redirs[i];
        int flags;
        if (r->mode == 0) flags = O_RDONLY;                          // < : lecture
        else if (r->mode == 1) flags = O_WRONLY | O_CREAT | O_TRUNC; // > : créer/vider
        else flags = O_WRONLY | O_CREAT | O_APPEND;                  // >> : ajouter à la fin
        int fd = open(r->file, flags, 0644);   // 0644 : droits si le fichier est créé
        if (fd < 0) { perror(r->file); return -1; }
        dup2(fd, r->target);   // le descripteur 0 ou 1 pointe maintenant sur le fichier
        close(fd);             // l'ancien numéro ne sert plus
    }
    return 0;
}

/* ---------- 1.D  Les builtins (jalon 2) ---------- */

/* Exécute un builtin et renvoie son code de retour (0 = succès). */
static int run_builtin(Command *c)
{
    char **a = c->argv;       // raccourci : a[0] = nom, a[1].. = arguments
    if (!strcmp(a[0], "go")) {                 // ---- go : change de dossier (cd)
        const char *dir = c->argc > 1 ? a[1] : var_get("HOME"); // sans argument : $HOME
        if (chdir(dir) < 0) { perror("go"); return 1; }         // appel système chdir
        char *cwd = getcwd(NULL, 0);           // nouveau dossier courant
        if (cwd) { setenv("PWD", cwd, 1); free(cwd); }
        return 0;
    }
    if (!strcmp(a[0], "quit")) {               // ---- quit : quitte le shell (exit)
        exit_requested = 1;
        exit_code = c->argc > 1 ? atoi(a[1]) : last_status;  // code donné, sinon $?
        return exit_code;
    }
    if (!strcmp(a[0], "path")) {               // ---- path : affiche le dossier (pwd)
        char *cwd = getcwd(NULL, 0);
        if (!cwd) { perror("path"); return 1; }
        puts(cwd);
        free(cwd);
        return 0;
    }
    if (!strcmp(a[0], "say")) {                // ---- say : affiche ses arguments (echo)
        for (int i = 1; i < c->argc; i++) {
            if (i > 1) putchar(' ');           // un espace entre les mots
            fputs(a[i], stdout);
        }
        putchar('\n');
        return 0;
    }
    if (!strcmp(a[0], "info")) {               // ---- info : aide (help)
        puts("ocsh - mini-shell pédagogique (projet 3, UIR/ESIN)");
        puts("Builtins : go quit path say info past osinfo export jobs fg bg");
        puts("Alias    : list show copy move del newdir newfile");
        puts("Syntaxe  : cmd1 -> cmd2   <  >  >>  &   ;");
        puts("Jobs     : cmd &   Ctrl+Z (pause)   jobs   fg [%n]   bg [%n]");
        puts("Variables: NOM=valeur  $NOM  ${NOM}  $?  $!  $$   export NOM");
        puts("Substit. : $(commande)  ou  `commande`");
        puts("Scripts  : if/then/else/fi   for x in ...; do ...; done   while ...; do ...; done");
        return 0;
    }
    if (!strcmp(a[0], "past")) {               // ---- past : historique (history)
        history_print();
        return 0;
    }
    if (!strcmp(a[0], "osinfo")) {             // ---- osinfo : texte imposé par le sujet
        puts("Systèmes d’Exploitation -- UIR/ESIN");
        return 0;
    }
    if (!strcmp(a[0], "export")) {             // ---- export : variable visible des programmes lancés
        var_export_words(c);
        return 0;
    }
    if (!strcmp(a[0], "jobs")) {               // ---- jobs : liste les tâches de fond / arrêtées
        jobs_print();
        return 0;
    }
    if (!strcmp(a[0], "fg")) return job_fg(c);  // ---- fg : un job passe au premier plan
    if (!strcmp(a[0], "bg")) return job_bg(c);  // ---- bg : un job arrêté repart en arrière-plan
    return 1;   // ne devrait jamais arriver (is_builtin a déjà vérifié)
}

/* ---------- 1.E  Signaux (jalon 3) ---------- */

/*  SIGINT (Ctrl+C) : le shell ne doit PAS mourir. Le gestionnaire note
 * seulement qu'un Ctrl+C a eu lieu. Les enfants, eux, remettent le comportement
 * par défaut (ils meurent). Avec le job control, chaque pipeline a son PROPRE
 * groupe de processus : le terminal envoie Ctrl+C seulement au groupe qui est
 * au premier plan (le job en cours), pas au shell. */
static void on_sigint(int sig)
{
    (void)sig;         // paramètre non utilisé
    interrupted = 1;   // on note juste l'événement (un gestionnaire doit rester court)
}

/* Vrai si tous les processus d'un job sont terminés (leur PID a été remis à 0). */
static int all_dead(const Job *j)
{
    for (int k = 0; k < j->npids; k++)
        if (j->pids[k] != 0) return 0;      // un processus vit encore
    return 1;
}

/* SIGCHLD : le noyau l'envoie au parent quand un enfant se termine OU
 * s'arrête. Un enfant terminé que personne n'a "attendu" reste ZOMBIE (entrée
 * dans la table des processus). Le gestionnaire appelle waitpid(-1, WNOHANG)
 * en boucle pour ramasser tous les enfants de fond, sans jamais bloquer, et met
 * à jour l'état du job concerné (Stopped ou Done). */
static void on_sigchld(int sig)
{
    (void)sig;
    int saved = errno;                  // un gestionnaire doit préserver errno
    int st;
    pid_t p;
    // -1 : n'importe quel enfant ; WNOHANG : ne bloque pas ; WUNTRACED : signale aussi les enfants ARRÊTÉS
    while ((p = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
        for (int i = 0; i < MAX_JOBS; i++) {          // à quel job appartient ce processus ?
            if (!jobs[i].used) continue;
            for (int k = 0; k < jobs[i].npids; k++) {
                if (jobs[i].pids[k] != p) continue;   // pas lui
                if (WIFSTOPPED(st)) {
                    jobs[i].state = JOB_STOPPED;      // il vient de s'arrêter (pause)
                } else {
                    jobs[i].pids[k] = 0;              // il est terminé : on l'oublie
                    if (all_dead(&jobs[i]))
                        jobs[i].state = JOB_DONE;     // tout le pipeline est fini
                }
            }
        }
    }
    errno = saved;
}

/* Installe les gestionnaires avec sigaction. */
static void signals_setup(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_sigint;
    sa.sa_flags = 0;                       // pas de SA_RESTART : read() est interrompu par Ctrl+C
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART;              // redémarre les appels interrompus ; pas de SA_NOCLDSTOP : on veut voir les arrêts
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGTSTP, SIG_IGN);              // Ctrl+Z ne doit pas figer le SHELL (il arrête les jobs : voir run_child)
    signal(SIGTTIN, SIG_IGN);              // sinon le shell serait arrêté quand il lit le clavier depuis l'arrière-plan
    signal(SIGTTOU, SIG_IGN);              // sinon tcsetpgrp() (reprendre le terminal) arrêterait le shell
    sigemptyset(&chld_set);
    sigaddset(&chld_set, SIGCHLD);         // ensemble {SIGCHLD}, utilisé pour le bloquer
}

/* ---------- 1.F  Attente et JOB CONTROL (option du jalon 6 : jobs, fg, bg, Ctrl+Z) ---------- */

/*  LES IDÉES  :
 *   - Un JOB = un pipeline (une ou plusieurs commandes liées par ->).
 *   - Chaque job a son PROPRE GROUPE de processus (setpgid) : on peut envoyer un
 *     signal à tout le groupe d'un coup avec kill(-pgid, signal).
 *   - Le TERMINAL a un groupe "au premier plan" (tcsetpgrp) : seul ce groupe reçoit
 *     le clavier, Ctrl+C et Ctrl+Z. Le shell donne le terminal au job de premier
 *     plan, puis le reprend quand le job finit ou s'arrête.
 *   - Ctrl+Z envoie SIGTSTP au job : il s'ARRÊTE (pause). SIGCONT le fait repartir.
 *        jobs : liste les jobs     fg : un job passe au premier plan
 *        bg : un job arrêté repart en arrière-plan */

/* Convertit le "status" brut de waitpid en code de retour de type shell. */
static int wait_status(int st)
{
    if (WIFEXITED(st)) return WEXITSTATUS(st);      // fin normale : le code du programme
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st); // tué par un signal : 128 + n° du signal
    return 1;
}

/*  Attend un job de PREMIER PLAN : tous les processus du groupe pgid.
 * waitpid(-pgid, ...) = "attends n'importe quel enfant du groupe pgid".
 * WUNTRACED : waitpid nous réveille aussi si un enfant S'ARRÊTE (Ctrl+Z).
 * pids[] : on remet à 0 le PID de chaque processus qui se termine.
 * last   : PID de la dernière commande (son code de retour est celui du job).
 * *stopped passe à 1 si le job s'est arrêté au lieu de se terminer. */
static int wait_group(pid_t pgid, pid_t last, volatile pid_t *pids, int n, int *stopped)
{
    int status = 0;
    *stopped = 0;
    for (;;) {
        int st;
        pid_t p = waitpid(-pgid, &st, WUNTRACED);
        if (p < 0) {
            if (errno == EINTR) {               // un signal a interrompu l'attente
                if (interrupted) kill(-pgid, SIGINT);   // Ctrl+C reçu par le shell (mode script) : on le transmet au job
                continue;                       // et on recommence à attendre
            }
            break;                              // ECHILD : plus aucun enfant dans ce groupe -> fini
        }
        if (WIFSTOPPED(st)) {                   // le job s'est arrêté (Ctrl+Z)
            *stopped = 1;
            status = 128 + WSTOPSIG(st);        // 128 + SIGTSTP (20) = 148
            break;
        }
        for (int k = 0; k < n; k++)             // ce processus est fini : on l'oublie
            if (pids[k] == p) pids[k] = 0;
        if (p == last) {                        // c'est la dernière commande : son code = code du job
            status = wait_status(st);
            if (WIFSIGNALED(st) && WTERMSIG(st) == SIGINT) {
                putchar('\n');                  // Ctrl+C : on passe à la ligne
                interrupted = 1;                // on le note : les boucles / la ligne en cours s'arrêtent
            }
        }
    }
    return status;
}

/* Range un nouveau job dans la table. Renvoie sa case (NULL si la table est pleine).
 * Appelée avec SIGCHLD bloqué : le gestionnaire ne peut pas lire la table à moitié remplie. */
static Job *job_add(const pid_t *pids, int n, const char *text, int state)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].used) continue;             // case déjà prise : suivante
        Job *j = &jobs[i];
        j->id = i + 1;                          // la case 0 est le job [1], etc.
        j->pgid = pids[0];                      // le groupe porte le PID de la 1re commande
        j->npids = n;
        for (int k = 0; k < n; k++) j->pids[k] = pids[k];
        j->last_pid = pids[n - 1];
        j->state = state;
        snprintf(j->cmd, sizeof j->cmd, "%s", text);   // copie (tronquée si trop longue)
        j->used = 1;                            // en DERNIER : la case devient visible
        return j;
    }
    return NULL;
}


static const char *state_name(int s)
{
    return s == JOB_RUNNING ? "Running" : s == JOB_STOPPED ? "Stopped" : "Done";
}

/* Affiche les jobs terminés (appelé avant chaque prompt) puis les oublie. */
static void print_done_notices(void)
{
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);    // pas de SIGCHLD pendant qu'on lit la table
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].used && jobs[i].state == JOB_DONE) {
            printf("[%d] Done      %s\n", jobs[i].id, jobs[i].cmd);
            jobs[i].used = 0;                   // case libérée
        }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* Builtin jobs : liste les jobs. */
static void jobs_print(void)
{
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used) continue;
        printf("[%d] %-8s %s\n", jobs[i].id, state_name(jobs[i].state), jobs[i].cmd);
        if (jobs[i].state == JOB_DONE) jobs[i].used = 0;   // un job fini est annoncé une seule fois
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* Trouve le job visé par "fg" / "bg" :  fg 2   fg %2   ou   fg (le dernier job). */
static Job *job_find(Command *c)
{
    int want = 0;                               // numéro demandé (0 = pas de numéro)
    if (c->argc > 1) {
        const char *s = c->argv[1];
        if (*s == '%') s++;                     // "%2" et "2" veulent dire la même chose
        want = atoi(s);
    }
    Job *last = NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].used || jobs[i].state == JOB_DONE) continue;
        if (want == 0) last = &jobs[i];                 // sans numéro : on retient le dernier de la table
        else if (jobs[i].id == want) return &jobs[i];   // avec numéro : c'est celui-là
    }
    return last;
}

/* Builtin fg : ramène un job au premier plan et l'attend. */
static int job_fg(Command *c)
{
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);    // on attend nous-mêmes : le gestionnaire ne doit pas "voler" le code de retour
    Job *j = job_find(c);
    if (!j) {
        fprintf(stderr, "fg: aucun job\n");
        sigprocmask(SIG_SETMASK, &old, NULL);
        return 1;
    }
    printf("%s\n", j->cmd);                     // comme Bash : on rappelle la commande
    fflush(stdout);
    j->state = JOB_RUNNING;
    if (interactive) tcsetpgrp(STDIN_FILENO, j->pgid);   // le job reçoit le clavier
    kill(-j->pgid, SIGCONT);                    // s'il était arrêté, on le relance (sans effet sinon)
    int stopped;
    int status = wait_group(j->pgid, j->last_pid, j->pids, j->npids, &stopped);
    if (interactive) tcsetpgrp(STDIN_FILENO, getpgrp()); // le shell reprend le clavier
    if (stopped) {                              // arrêté de nouveau (Ctrl+Z) : on le garde dans la table
        j->state = JOB_STOPPED;
        printf("\n[%d]+ Stopped   %s\n", j->id, j->cmd);
        interrupted = 1;                        // le reste de la ligne est abandonné
    } else {
        j->used = 0;                            // terminé : on libère la case
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return status;
}

/* Builtin bg : relance un job ARRÊTÉ en arrière-plan (sans l'attendre). */
static int job_bg(Command *c)
{
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);
    Job *j = job_find(c);
    int st = 0;
    if (!j) {
        fprintf(stderr, "bg: aucun job\n");
        st = 1;
    } else if (j->state != JOB_STOPPED) {
        fprintf(stderr, "bg: le job %d tourne déjà\n", j->id);
        st = 1;
    } else {
        j->state = JOB_RUNNING;
        kill(-j->pgid, SIGCONT);                // SIGCONT à tout le groupe : il repart
        printf("[%d] %s &\n", j->id, j->cmd);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    return st;
}

/* ---------- 1.G  Lancement : fork + pipe + dup2 + execvp + waitpid ---------- */

/* Code exécuté par l'ENFANT (après fork) pour la commande numéro i du pipeline.
 * pgid : groupe du pipeline (0 pour la 1re commande = elle crée le groupe).
 * Ne retourne jamais : l'enfant fait exec ou _exit. */
static void run_child(Command *c, int i, int n, int fds[][2], int background, sigset_t *old, pid_t pgid)
{
    /* Groupe de processus : chaque pipeline a son propre groupe. */
    setpgid(0, pgid);                           // 0 : je crée un nouveau groupe (je suis la 1re commande) ; sinon je rejoins celui de la 1re
    if (interactive && !background)
        tcsetpgrp(STDIN_FILENO, getpgrp());     // premier plan : mon groupe prend le clavier (sinon SIGTTIN)

    /* Remettre les signaux comme un programme normal les attend. */
    signal(SIGINT, SIG_DFL);                    // Ctrl+C tue l'enfant (pas le shell)
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);                   // Ctrl+Z peut arrêter l'enfant (le shell, lui, l'ignore)
    signal(SIGTTIN, SIG_DFL);                   // SIG_IGN est hérité par exec : on le remet par défaut
    signal(SIGTTOU, SIG_DFL);
    sigprocmask(SIG_SETMASK, old, NULL);        // débloque SIGCHLD (le masque est hérité par exec !)

    /* Brancher les pipes. Le pipe a deux bouts : fds[k][0] lecture, fds[k][1] écriture.
     * Commande i lit ce que la commande i-1 écrit, et écrit pour la commande i+1. */
    int has_input = 0;
    for (int k = 0; k < c->nredir; k++)
        if (c->redirs[k].target == 0) has_input = 1;
    if (i > 0)
        dup2(fds[i - 1][0], 0);                 // stdin  <- sortie du pipe précédent
    else if (background && !has_input) {        // 1re commande en fond : pas de clavier
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        close(nul);
    }
    if (i < n - 1)
        dup2(fds[i][1], 1);                     // stdout -> entrée du pipe suivant
    for (int k = 0; k < n - 1; k++) {           // IMPORTANT : fermer TOUS les bouts de pipe,
        close(fds[k][0]);                       // sinon le lecteur ne verrait jamais la fin
        close(fds[k][1]);                       // de fichier (EOF) et attendrait pour toujours
    }

    /* Redirections > >> < (elles l'emportent sur les pipes). */
    if (apply_redirs(c) < 0)
        _exit(1);

    /* Soit un builtin (dans l'enfant, utile dans un pipe), soit un vrai programme. */
    if (is_builtin(c->argv[0])) {
        int st = run_builtin(c);
        fflush(stdout);                         // vide le tampon avant _exit (qui ne le fait pas)
        _exit(st);
    }
    const char *shown = c->argv[0];             // nom tapé (pour le message d'erreur)
    c->argv[0] = (char *)resolve_alias(c->argv[0]);   // list -> ls, etc.
    execvp(c->argv[0], c->argv);                // REMPLACE le programme de l'enfant par celui-ci
    /* Si on arrive ici, exec a ÉCHOUÉ (sinon le code de l'enfant n'existe plus). */
    if (errno == ENOENT)
        fprintf(stderr, "ocsh: %s: commande introuvable\n", shown);
    else
        fprintf(stderr, "ocsh: %s: %s\n", shown, strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);         // 127 : introuvable ; 126 : non exécutable
}

/* Lance un pipeline de n commandes. Renvoie le code de retour (de la dernière).
 * text : la commande telle qu'elle a été tapée (pour l'afficher dans "jobs"). */
static int launch_pipeline(Command *cmds, int n, int background, const char *text)
{
    fflush(NULL);   // vide les tampons AVANT fork, sinon l'enfant recopierait du texte en attente

    /* CAS 1 : un builtin seul, au premier plan -> exécuté dans le PARENT.
     * (go, quit, export, jobs, fg, bg doivent agir sur le shell lui-même.) On sauvegarde 0 et 1
     * avec dup(), on applique les redirections, puis on restaure. */
    if (n == 1 && !background && is_builtin(cmds[0].argv[0])) {
        int save0 = dup(0), save1 = dup(1);     // copies des vrais stdin/stdout
        int st = apply_redirs(&cmds[0]) < 0 ? 1 : run_builtin(&cmds[0]);
        fflush(stdout);                         // écrit dans le fichier AVANT de restaurer
        dup2(save0, 0);                         // on remet le vrai clavier
        dup2(save1, 1);                         // on remet le vrai écran
        close(save0);
        close(save1);
        return st;
    }

    /* CAS 2 : le cas général. On crée n-1 pipes, puis n enfants. */
    int fds[MAX_PIPE][2];
    pid_t pids[MAX_PIPE];
    for (int i = 0; i < n - 1; i++)
        if (pipe(fds[i]) < 0) {                 // pipe() : crée un canal fds[i][0] (lecture) / fds[i][1] (écriture)
            perror("pipe");
            return 1;
        }
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);    // bloque SIGCHLD : le gestionnaire ne doit pas
                                                // "voler" le code de retour d'un enfant de premier plan
    pid_t pgid = 0;                             // groupe du pipeline (0 = pas encore créé)
    int started = 0;                            // nombre d'enfants réellement créés
    for (int i = 0; i < n; i++) {
        pid_t pid = fork();                     // duplique le processus : 2 processus à partir d'ici
        if (pid < 0) { perror("fork"); break; }
        if (pid == 0)
            run_child(&cmds[i], i, n, fds, background, &old, pgid);   // ENFANT (fork a renvoyé 0)
        if (pgid == 0) pgid = pid;              // PARENT : la 1re commande est le chef du groupe
        setpgid(pid, pgid);                     // le parent range aussi l'enfant dans le groupe (évite une course avec l'enfant)
        pids[started++] = pid;
    }
    for (int i = 0; i < n - 1; i++) {           // le parent n'utilise pas les pipes : il les ferme
        close(fds[i][0]);
        close(fds[i][1]);
    }

    int status = 0;
    if (started == 0) {                         // aucun fork n'a marché
        status = 1;
    } else if (background) {                    // [BLOC] arrière-plan : on N'ATTEND PAS
        last_bg = pids[started - 1];            // $! = PID de la dernière commande
        Job *j = job_add(pids, started, text, JOB_RUNNING);   // on retient le job (pour jobs / fg / bg)
        if (!j) fprintf(stderr, "ocsh: trop de jobs, celui-ci ne sera pas suivi\n");
        else if (interactive)
            printf("[%d] %d\n", j->id, (int)last_bg);
    } else {                                    // [BLOC] premier plan : on attend le job
        if (interactive) tcsetpgrp(STDIN_FILENO, pgid);      // le job reçoit le clavier
        int stopped;
        status = wait_group(pgid, pids[started - 1], pids, started, &stopped);
        if (interactive) tcsetpgrp(STDIN_FILENO, getpgrp()); // le shell reprend le clavier
        if (stopped) {                          // Ctrl+Z : le job est en pause, on le range dans la table
            Job *j = job_add(pids, started, text, JOB_STOPPED);
            if (j) printf("\n[%d]+ Stopped   %s\n", j->id, j->cmd);
            interrupted = 1;                    // le reste de la ligne est abandonné
        }
        if (started < n) status = 1;            // un fork a échoué
    }
    sigprocmask(SIG_SETMASK, &old, NULL);       // on débloque SIGCHLD (zombies de fond ramassés)
    return status;
}

/* ---------- 1.H  Exécution d'UNE instruction (colle partie 1 et partie 2) ---------- */

/* Vrai si le mot est de la forme NOM=valeur (renvoie un pointeur sur le '='). */
static char *is_assignment(char *w)
{
    if (!(isalpha((unsigned char)w[0]) || w[0] == '_'))
        return NULL;                            // un nom commence par une lettre ou _
    char *p = w;
    while (isalnum((unsigned char)*p) || *p == '_') p++;   // avance dans le nom
    return *p == '=' ? p : NULL;
}

/* Exécute UNE instruction (pas de ';' dedans) : expansion, découpage sur ->,
 * analyse, lancement. Renvoie son code de retour. */
static int exec_statement(const char *stmt)
{
    char *e = expand(stmt);                     // étape 2 : $VAR, $?, $(...) remplacés (partie 2)

    /* Un '&' final = arrière-plan. On le retire du texte. */
    size_t l = strlen(e);
    while (l && isspace((unsigned char)e[l - 1])) e[--l] = '\0';   // retire les espaces de fin
    int bg = 0;
    if (l && e[l - 1] == '&') { bg = 1; e[--l] = '\0'; }
    while (l && isspace((unsigned char)e[l - 1])) e[--l] = '\0';   // retire les espaces avant le '&'

    char text[128];                             // copie du texte COMPLET (pour "jobs") : split_pipeline va le découper
    snprintf(text, sizeof text, "%s", e);

    /* Découpage sur "->" puis analyse de chaque case. */
    char *segs[MAX_PIPE];
    int n = split_pipeline(e, segs, MAX_PIPE);
    if (n < 0) {
        fprintf(stderr, "ocsh: trop de commandes dans le pipeline\n");
        free(e);
        return last_status = 2;
    }
    Command cmds[MAX_PIPE];
    memset(cmds, 0, sizeof cmds);               // tout à zéro : argc = 0, nredir = 0...
    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        if (parse_command(segs[i], &cmds[i]) < 0)
            ok = 0;
        else if (cmds[i].argc == 0 && n > 1) {  // "a -> -> b" ou "a ->" : case vide
            fprintf(stderr, "ocsh: erreur de syntaxe : commande attendue\n");
            ok = 0;
        }
    }
    int st;
    if (!ok) {
        st = 2;                                 // 2 = erreur de syntaxe
    } else if (n == 1 && cmds[0].argc == 0) {
        st = last_status;                       // ligne vide (ou variable vide) : on ne fait rien
    } else if (n == 1 && cmds[0].argc == 1 && cmds[0].nredir == 0 && !bg
               && is_assignment(cmds[0].argv[0])) {
        char *eq = is_assignment(cmds[0].argv[0]);   // affectation  NOM=valeur
        *eq = '\0';                                  // coupe : argv[0] = nom, eq+1 = valeur
        var_set(cmds[0].argv[0], eq + 1, 0);
        st = 0;
    } else {
        st = launch_pipeline(cmds, n, bg, text); // le vrai travail : fork/exec/wait
    }
    for (int i = 0; i < n; i++) free_command(&cmds[i]);
    free(e);
    return last_status = st;                    // $? = code de cette commande
}