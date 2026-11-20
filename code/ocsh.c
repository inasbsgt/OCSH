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
/* =====================================================================
 *                         PARTIE 2  (jalons 4, 5, 6)
 *   Variables et expansion, $(...) et `...`, .ocshrc, historique,
 *   complétion, mode script (if / for / while), programme principal
 * ===================================================================== */

/* ---------- 2.A  Variables du shell (jalon 4) ---------- */

/* Une variable : un nom, une valeur, et un drapeau "exportée". */
typedef struct {
    char *name, *value;
    int exported;      // 1 : copiée dans l'environnement (visible des programmes lancés)
} Var;
static Var vars[MAX_VARS];
static int nvars = 0;

/* Cherche une variable par son nom (NULL si absente). */
static Var *var_find(const char *name)
{
    for (int i = 0; i < nvars; i++)
        if (!strcmp(vars[i].name, name))
            return &vars[i];
    return NULL;
}
/* Valeur d'une variable : table du shell, sinon environnement, sinon "". */
static const char *var_get(const char *name)
{
    Var *v = var_find(name);
    if (v) return v->value;
    const char *env = getenv(name);    // ex. HOME, PATH viennent de l'environnement
    return env ? env : "";
}
/* Crée ou modifie une variable. exported = 1 pour la rendre visible aux programmes. */
static void var_set(const char *name, const char *value, int exported)
{
    Var *v = var_find(name);
    if (!v) {                                    // nouvelle variable
        if (nvars >= MAX_VARS) { fprintf(stderr, "ocsh: trop de variables\n"); return; }
        v = &vars[nvars++];
        v->name = xstrdup(name);
        v->value = NULL;
        v->exported = 0;
    }
    free(v->value);                              // libère l'ancienne valeur
    v->value = xstrdup(value);
    if (exported) v->exported = 1;
    if (v->exported) setenv(name, value, 1);     // copie dans l'environnement (hérité par exec)
}
/* Builtin export :  export NOM=valeur   ou   export NOM  */
static void var_export_words(Command *c)
{
    for (int i = 1; i < c->argc; i++) {
        char *eq = is_assignment(c->argv[i]);
        if (eq) {                                // NOM=valeur
            *eq = '\0';
            var_set(c->argv[i], eq + 1, 1);
        } else {                                 // NOM seul : on exporte la valeur actuelle
            var_set(c->argv[i], var_get(c->argv[i]), 1);
        }
    }
}

/* ---------- 2.B  Substitution de commande $(...) (jalon 5) ---------- */

/* Exécute cmd dans un ENFANT dont la sortie est reliée à un pipe, lit le pipe
 * et renvoie le texte produit (sans les retours à la ligne finaux). */
static char *command_subst(const char *cmd)
{
    fflush(NULL);
    int fd[2];
    if (pipe(fd) < 0) { perror("pipe"); return xstrdup(""); }
    sigset_t old;
    sigprocmask(SIG_BLOCK, &chld_set, &old);     
    pid_t pid = fork();
    if (pid == 0) {                              
        interactive = 0;                         
        signal(SIGINT, SIG_DFL);
        sigprocmask(SIG_SETMASK, &old, NULL);
        close(fd[0]);                            // l'enfant n'a pas besoin du bout de lecture
        dup2(fd[1], 1);                          // sa sortie standard va dans le pipe
        close(fd[1]);
        run_source(cmd);                         // exécute la commande
        fflush(stdout);
        _exit(last_status);
    }
    close(fd[1]);                                // PARENT : ferme le bout d'écriture (sinon EOF jamais vu)
    Buf out = {0};
    char tmp[512];
    ssize_t r;
    while ((r = read(fd[0], tmp, sizeof tmp)) > 0 || (r < 0 && errno == EINTR))
        if (r > 0) buf_addn(&out, tmp, (size_t)r);   // lit tout ce que l'enfant a écrit
    close(fd[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    sigprocmask(SIG_SETMASK, &old, NULL);
    char *res = buf_take(&out);
    size_t l = strlen(res);
    while (l && res[l - 1] == '\n') res[--l] = '\0';  
    return res;
}

/* ---------- 2.C  Expansion des variables (jalon 4) ---------- */

/* expand : parcourt le texte et remplace
 *   $NOM  ${NOM}  $?  $!  $$   par leur valeur,
 *   $(cmd)  et  `cmd`          par la sortie de cmd.
 * Règles de guillemets : '...' = rien n'est remplacé ; "..." = remplacé.
 * Les guillemets sont CONSERVÉS ici : parse_word les enlèvera plus tard.
 * L'expansion se fait au DERNIER moment (à chaque exécution), donc dans une
 * boucle, $x prend la valeur courante à chaque tour. */
static char *expand(const char *in)
{
    Buf out = {0};
    int in_dq = 0;                               // 1 si on est dans des "..."
    const char *p = in;
    while (*p) {
        char c = *p;
        if (c == '\\' && p[1]) {                 // \x : on garde tel quel (parse_word gèrera)
            buf_addc(&out, c); buf_addc(&out, p[1]); p += 2;
        } else if (c == '\'' && !in_dq) {        // '...' : copie sans rien remplacer
            const char *end = strchr(p + 1, '\'');
            size_t len = end ? (size_t)(end - p) + 1 : strlen(p);
            buf_addn(&out, p, len);
            p += len;
        } else if (c == '"') {                   // ouvre/ferme les guillemets doubles
            in_dq = !in_dq; buf_addc(&out, c); p++;
        } else if (c == '$' && p[1] == '(') {    // $(commande)
            int depth = 1;
            const char *q = p + 2;
            while (*q && depth) {                // cherche la ')' qui correspond
                if (*q == '(') depth++;
                else if (*q == ')') depth--;
                q++;
            }
            char *inner = slice(p + 2, (size_t)(q - (p + 2)) - (depth == 0 ? 1 : 0));
            char *res = command_subst(inner);    // exécute et récupère la sortie
            buf_add(&out, res);
            free(res); free(inner);
            p = q;
        } else if (c == '`') {                   // `commande`
            const char *q = strchr(p + 1, '`');
            size_t len = q ? (size_t)(q - (p + 1)) : strlen(p + 1);
            char *inner = slice(p + 1, len);
            char *res = command_subst(inner);
            buf_add(&out, res);
            free(res); free(inner);
            p += len + (q ? 2 : 1);
        } else if (c == '$' && p[1] == '{') {    // ${NOM}
            const char *q = strchr(p + 2, '}');
            if (!q) { buf_addc(&out, c); p++; continue; }   // pas de '}' : '$' littéral
            char *name = slice(p + 2, (size_t)(q - (p + 2)));
            buf_add(&out, var_get(name));
            free(name);
            p = q + 1;
        } else if (c == '$' && (p[1] == '?' || p[1] == '!' || p[1] == '$')) {   // $? $! $$
            char num[32];
            int v = p[1] == '?' ? last_status : p[1] == '!' ? (int)last_bg : (int)getpid();
            snprintf(num, sizeof num, "%d", v);
            buf_add(&out, num);
            p += 2;
        } else if (c == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {  // $NOM
            const char *q = p + 1;
            while (isalnum((unsigned char)*q) || *q == '_') q++;   // le nom s'arrête au 1er caractère interdit
            char *name = slice(p + 1, (size_t)(q - (p + 1)));
            buf_add(&out, var_get(name));
            free(name);
            p = q;
        } else {                                 // caractère ordinaire : copié tel quel
            buf_addc(&out, c);
            p++;
        }
    }
    return buf_take(&out);
}

/* ---------- 2.D  Historique (jalon 5) ---------- */

static char *hist[MAX_HIST];   // les commandes déjà tapées
static int hist_n = 0;         // combien il y en a

static void history_add(const char *line)
{
    if (!*line) return;                                  // ligne vide : ignorée
    if (hist_n && !strcmp(hist[hist_n - 1], line)) return;   // même que la précédente : ignorée
    if (hist_n == MAX_HIST) {                            // plein : on oublie la plus ancienne
        free(hist[0]);
        memmove(hist, hist + 1, (MAX_HIST - 1) * sizeof hist[0]);
        hist_n--;
    }
    hist[hist_n++] = xstrdup(line);
}
static void history_print(void)                          // builtin "past"
{
    for (int i = 0; i < hist_n; i++)
        printf("%4d  %s\n", i + 1, hist[i]);
}
/* Chemin d'un fichier dans $HOME (renvoie NULL si HOME est vide). Libérer avec free. */
static char *home_file(const char *name)
{
    const char *home = var_get("HOME");
    if (!*home) return NULL;
    Buf b = {0};
    buf_add(&b, home);
    buf_addc(&b, '/');
    buf_add(&b, name);
    return buf_take(&b);
}
static void history_load(void)                           // relit ~/.ocsh_history
{
    char *path = home_file(".ocsh_history");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t r;
        while ((r = getline(&line, &cap, f)) > 0) {      // getline alloue la ligne pour nous
            if (line[r - 1] == '\n') line[r - 1] = '\0';
            history_add(line);
        }
        free(line);
        fclose(f);
    }
    free(path);
}
static void history_save(void)                           // sauvegarde dans ~/.ocsh_history
{
    char *path = home_file(".ocsh_history");
    FILE *f = path ? fopen(path, "w") : NULL;
    if (f) {
        for (int i = 0; i < hist_n; i++)
            fprintf(f, "%s\n", hist[i]);
        fclose(f);
    }
    free(path);
}

/* ---------- 2.E  Saisie au clavier : flèches et complétion Tab (jalon 5) ---------- */

/* Ajoute une proposition de complétion si elle commence par prefix. */
static void cand_add(char **cand, int *nc, const char *full, const char *prefix, const char *suffix)
{
    if (*nc >= MAX_CAND || strncmp(full, prefix, strlen(prefix)) != 0) return;
    Buf b = {0};
    buf_add(&b, full);
    buf_add(&b, suffix);
    cand[(*nc)++] = buf_take(&b);
}

/* Complétion : quand on appuie sur Tab, on regarde le dernier mot tapé.
 *   - premier mot  -> noms de nos commandes (builtins + alias)
 *   - sinon        -> fichiers/dossiers du dossier concerné
 * 1 seule proposition : on complète. Plusieurs : on complète le début commun,
 * et si rien n'avance, on affiche la liste. */
static void complete(char *buf, int *len)
{
    int start = *len;
    while (start > 0 && buf[start - 1] != ' ') start--;   // début du dernier mot
    char *word = slice(buf + start, (size_t)(*len - start));
    int first = 1;                                        // est-ce le premier mot ?
    for (int i = 0; i < start; i++)
        if (buf[i] != ' ') first = 0;
    char *cand[MAX_CAND];
    int nc = 0;

    if (first && !strchr(word, '/')) {                    // commandes
        for (int i = 0; builtins[i]; i++) cand_add(cand, &nc, builtins[i], word, " ");
        for (size_t i = 0; i < N_ALIASES; i++) cand_add(cand, &nc, aliases[i][0], word, " ");
    }
    /* fichiers : on sépare "dossier/préfixe" au dernier '/' */
    char *slash = strrchr(word, '/');
    char *dir = slash ? slice(word, (size_t)(slash - word) + 1) : xstrdup("");  // "src/" ou ""
    const char *prefix = slash ? slash + 1 : word;
    if (!first || slash) {
        DIR *d = opendir(*dir ? dir : ".");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {                   // un tour par fichier du dossier
                if (de->d_name[0] == '.' && prefix[0] != '.') continue;   // fichiers cachés : seulement si demandés
                Buf full = {0};
                buf_add(&full, dir);
                buf_add(&full, de->d_name);
                struct stat sb;
                int isdir = stat(full.s, &sb) == 0 && S_ISDIR(sb.st_mode);
                char *name = buf_take(&full);
                if (!strncmp(de->d_name, prefix, strlen(prefix)))
                    cand_add(cand, &nc, name, name, isdir ? "/" : " ");   // dossier -> "/", fichier -> espace
                free(name);
            }
            closedir(d);
        }
    }
    if (nc == 1) {                                        // une seule proposition : on complète
        if (start + (int)strlen(cand[0]) < MAX_LINE) {
            strcpy(buf + start, cand[0]);
            *len = start + (int)strlen(cand[0]);
        }
    } else if (nc > 1) {                                  // plusieurs : début commun
        size_t common = strlen(cand[0]);
        for (int i = 1; i < nc; i++) {
            size_t k = 0;
            while (k < common && cand[i][k] == cand[0][k]) k++;
            common = k;
        }
        if (common > strlen(word) && start + (int)common < MAX_LINE) {
            memcpy(buf + start, cand[0], common);         // on avance jusqu'au début commun
            *len = start + (int)common;
            buf[*len] = '\0';
        } else {                                          // rien à avancer : on liste
            putchar('\n');
            for (int i = 0; i < nc; i++) printf("%s  ", cand[i]);
            putchar('\n');
        }
    }
    for (int i = 0; i < nc; i++) free(cand[i]);
    free(dir);
    free(word);
}

/* Compte les CARACTÈRES (pas les octets) de buf[from..to-1] : un accent UTF-8
 * occupe 2 octets mais 1 seule colonne à l'écran. Les octets de continuation
 * (de la forme 10xxxxxx) ne comptent pas. Sert à replacer le curseur. */
static int nb_cols(const char *buf, int from, int to)
{
    int n = 0;
    for (int i = from; i < to; i++)
        if (((unsigned char)buf[i] & 0xC0) != 0x80) n++;
    return n;
}

/* Lecture d'une ligne au clavier, touche par touche.
 * On désactive le mode "canonique" (ICANON) et l'écho du terminal pour recevoir
 * chaque touche tout de suite (nécessaire pour Tab et les flèches). ISIG reste
 * actif : Ctrl+C envoie toujours SIGINT. Renvoie NULL pour Ctrl+D (fin) ou
 * Ctrl+C (alors interrupted vaut 1). */
static char *read_line(const char *prompt)
{
    struct termios orig, raw;
    tcgetattr(STDIN_FILENO, &orig);                       // sauvegarde les réglages du terminal
    raw = orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);            // plus de ligne entière, plus d'écho
    raw.c_cc[VMIN] = 1;                                   // read rend la main dès 1 caractère
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    char buf[MAX_LINE] = "", saved[MAX_LINE] = "";
    int len = 0, hpos = hist_n;                           // hpos : position dans l'historique
    int pos = 0;                                          // pos : position du curseur dans buf (0 = début, len = fin)
    char *result = NULL;
    for (;;) {
        printf("\r\x1b[2K%s%s", prompt, buf);             // efface la ligne et la redessine
        int back = nb_cols(buf, pos, len);                // combien de caractères entre le curseur et la fin ?
        if (back > 0) printf("\x1b[%dD", back);           // on ramène le curseur à sa place (ESC [ n D = n colonnes à gauche)
        fflush(stdout);
        unsigned char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);            // lit UNE touche
        if (r < 0) {
            if (errno == EINTR && interrupted) { printf("^C\n"); break; }  // Ctrl+C
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;                                // fin d'entrée
        if (c == '\n') { putchar('\n'); result = xstrdup(buf); break; }    // Entrée
        if (c == 4) {                                     // Ctrl+D
            if (len == 0) { putchar('\n'); break; }       // ligne vide : fin du shell
            continue;
        }
        if (c == 127 || c == 8) {                          // Retour arrière : efface le caractère AVANT le curseur
            if (pos > 0) {
                int start = pos - 1;
                while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80) start--;   // recule jusqu'au début du caractère (accent = 2 octets)
                memmove(buf + start, buf + pos, (size_t)(len - pos + 1));                  // décale la fin de la ligne vers la gauche ('\0' compris)
                len -= pos - start;
                pos = start;
            }
        }
        else if (c == '\t') { complete(buf, &len); pos = len; }            // Tab (la complétion se fait en fin de ligne)
        else if (c == 27) {                                // Échap : début d'une flèche (ESC [ A)
            unsigned char s1 = 0, s2 = 0;
            if (read(STDIN_FILENO, &s1, 1) <= 0 || read(STDIN_FILENO, &s2, 1) <= 0) continue;
            if (s1 == '[' && s2 == 'D') {                  // flèche GAUCHE : curseur d'un caractère vers le début
                if (pos > 0) {
                    pos--;
                    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos--;   // saute les octets d'un accent
                }
            } else if (s1 == '[' && s2 == 'C') {           // flèche DROITE : curseur d'un caractère vers la fin
                if (pos < len) {
                    pos++;
                    while (pos < len && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos++;
                }
            } else if (s1 == '[' && s2 == '3') {           // touche Suppr : ESC [ 3 ~ (on lit le '~' final)
                unsigned char tilde;
                if (read(STDIN_FILENO, &tilde, 1) > 0 && pos < len) {
                    int end = pos + 1;
                    while (end < len && ((unsigned char)buf[end] & 0xC0) == 0x80) end++;   // fin du caractère sous le curseur
                    memmove(buf + pos, buf + end, (size_t)(len - end + 1));                // efface-le
                    len -= end - pos;
                }
            } else if (s1 == '[' && s2 == 'A' && hpos > 0) {      // flèche haut : commande précédente
                if (hpos == hist_n) strcpy(saved, buf);    // garde ce qu'on tapait
                hpos--;
                strncpy(buf, hist[hpos], MAX_LINE - 1);
                buf[MAX_LINE - 1] = '\0';
                len = (int)strlen(buf);
                pos = len;                                 // curseur en fin de ligne
            } else if (s1 == '[' && s2 == 'B' && hpos < hist_n) {   // flèche bas : suivante
                hpos++;
                strncpy(buf, hpos == hist_n ? saved : hist[hpos], MAX_LINE - 1);
                buf[MAX_LINE - 1] = '\0';
                len = (int)strlen(buf);
                pos = len;                                 // curseur en fin de ligne
            }
        } else if (c >= 32 && len < MAX_LINE - 1) {       // caractère normal (accents UTF-8 inclus)
            memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));   // fait de la place au curseur ('\0' compris)
            buf[pos++] = (char)c;                          // insère le caractère, le curseur avance
            len++;
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &orig);              // IMPORTANT : on rend le terminal normal
    return result;
}

/* ---------- 2.F  Découpage en instructions et blocs if/for/while (jalon 6) ---------- */

static void stmts_append(Stmts *s, char *owned)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = xrealloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    s->v[s->n++] = owned;
}
/* Ajoute une instruction (espaces retirés). Si elle commence par then/else/do
 * suivi d'une commande ("then say yes"), on sépare : "then" puis "say yes".
 * Ainsi les blocs sont toujours "mot-clé seul" + instructions. */
static void stmts_push(Stmts *s, const char *text)
{
    while (isspace((unsigned char)*text)) text++;
    size_t l = strlen(text);
    while (l && isspace((unsigned char)text[l - 1])) l--;
    if (!l) return;                                      // instruction vide : ignorée
    char *t = slice(text, l);
    const char *kws[] = {"then", "else", "do", NULL};
    for (int k = 0; kws[k]; k++) {
        size_t kl = strlen(kws[k]);
        if (!strncmp(t, kws[k], kl) && isspace((unsigned char)t[kl])) {
            stmts_append(s, xstrdup(kws[k]));            // le mot-clé seul
            stmts_push(s, t + kl);                       // puis le reste
            free(t);
            return;
        }
    }
    stmts_append(s, t);
}

/* Coupe un texte (script ou ligne) en instructions, aux ';', aux retours
 * à la ligne et après chaque '&'. On ne coupe PAS à l'intérieur de guillemets,
 * de $( ) ou de `...`. Les commentaires (# jusqu'à la fin de ligne) sont ignorés.
 * *unterminated = 1 si un guillemet / $( / ` reste ouvert. */
static void split_statements(const char *src, Stmts *out, int *unterminated)
{
    Buf cur = {0};
    char q = 0;                 // guillemet ouvert
    int paren = 0, bt = 0;      // profondeur de $( ) ; dans des `...` ?
    for (const char *p = src; *p; p++) {
        char c = *p;
        if (q) {                                          // dans des guillemets : tout est copié
            buf_addc(&cur, c);
            if (c == '\\' && q == '"' && p[1]) buf_addc(&cur, *++p);
            else if (c == q) q = 0;
            continue;
        }
        if (c == '\\' && p[1]) { buf_addc(&cur, c); buf_addc(&cur, *++p); continue; }
        if (c == '\'' || c == '"') { q = c; buf_addc(&cur, c); continue; }
        if (c == '`') { bt = !bt; buf_addc(&cur, c); continue; }
        if (c == '$' && p[1] == '(') { paren++; buf_addc(&cur, c); buf_addc(&cur, *++p); continue; }
        if (c == ')' && paren > 0) { paren--; buf_addc(&cur, c); continue; }
        if (paren > 0 || bt) { buf_addc(&cur, c); continue; }   // dans $( ) ou `...` : copié
        if (c == '#' && (cur.len == 0 || isspace((unsigned char)cur.s[cur.len - 1]))) {
            while (p[1] && p[1] != '\n') p++;             // commentaire : saute jusqu'à la fin de ligne
            continue;
        }
        if (c == ';' || c == '\n' || c == '&') {          // séparateur d'instructions
            if (c == '&') buf_addc(&cur, c);              // le '&' reste (il signifie arrière-plan)
            stmts_push(out, cur.s ? cur.s : "");
            cur.len = 0;
            if (cur.s) cur.s[0] = '\0';
            continue;
        }
        buf_addc(&cur, c);
    }
    if (unterminated) *unterminated = (q != 0) || paren > 0 || bt;
    stmts_push(out, cur.s ? cur.s : "");
    free(cur.s);
}

static void stmts_free(Stmts *s)
{
    for (int i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = s->cap = 0;
}

/* Vrai si l'instruction s commence par le mot kw (suivi d'un espace ou de la fin). */
static int first_word_is(const char *s, const char *kw)
{
    size_t l = strlen(kw);
    return !strncmp(s, kw, l) && (s[l] == '\0' || isspace((unsigned char)s[l]));
}
static int is_opener(const char *s) { return first_word_is(s, "if") || first_word_is(s, "for") || first_word_is(s, "while"); }
static int is_closer(const char *s) { return first_word_is(s, "fi") || first_word_is(s, "done"); }

/* Trouve le "fi" / "done" qui ferme le bloc ouvert à l'indice i (en comptant
 * les blocs imbriqués). Renvoie -1 s'il manque. */
static int find_end(Stmts *st, int i, int to)
{
    int depth = 0;
    for (int k = i; k < to; k++) {
        if (is_opener(st->v[k])) depth++;
        else if (is_closer(st->v[k]) && --depth == 0) return k;
    }
    return -1;
}
/* Trouve le "else" du niveau courant entre then_idx et fi_idx (-1 s'il n'y en a pas). */
static int find_else(Stmts *st, int then_idx, int fi_idx)
{
    int depth = 0;
    for (int k = then_idx + 1; k < fi_idx; k++) {
        if (is_opener(st->v[k])) depth++;
        else if (is_closer(st->v[k])) depth--;
        else if (depth == 0 && !strcmp(st->v[k], "else")) return k;
    }
    return -1;
}

static int syntax_error(const char *near)
{
    fprintf(stderr, "ocsh: erreur de syntaxe près de « %s »\n", near);
    return last_status = 2;
}

/* Exécute les instructions st[from .. to-1].
 * Pour if/while/for : l'ARBRE n'existe pas, on lit directement les instructions
 * et on s'appelle récursivement sur le corps du bloc.
 *   if COND ; then A ; else B ; fi   -> COND est une commande : code 0 = vrai
 *   while COND ; do A ; done         -> on ré-exécute COND à chaque tour
 *   for x in a b c ; do A ; done     -> x prend chaque valeur */
static int exec_range(Stmts *st, int from, int to)
{
    int status = last_status;
    int i = from;
    while (i < to && !exit_requested && !interrupted) {   
        const char *s = st->v[i];
        if (first_word_is(s, "if")) {
            int fi = find_end(st, i, to);
            if (fi < 0 || i + 1 >= fi || strcmp(st->v[i + 1], "then")) return syntax_error(s);
            int el = find_else(st, i + 1, fi);
            int cond = exec_statement(s + 2);            // la condition est une commande normale
            if (cond == 0)
                status = exec_range(st, i + 2, el >= 0 ? el : fi);   // branche "then"
            else if (el >= 0)
                status = exec_range(st, el + 1, fi);                  // branche "else"
            else
                status = 0;
            i = fi + 1;                                  // on continue après le fi
        } else if (first_word_is(s, "while")) {
            int done = find_end(st, i, to);
            if (done < 0 || i + 1 >= done || strcmp(st->v[i + 1], "do")) return syntax_error(s);
            while (!exit_requested && !interrupted && exec_statement(s + 5) == 0)   // condition ré-évaluée à chaque tour ; Ctrl+C l'arrête
                status = exec_range(st, i + 2, done);
            i = done + 1;
        } else if (first_word_is(s, "for")) {
            int done = find_end(st, i, to);
            if (done < 0 || i + 1 >= done || strcmp(st->v[i + 1], "do")) return syntax_error(s);
            char *e = expand(s + 3);                     // "x in a b c" avec variables remplacées
            const char *p = e;
            char *words[MAX_ARGS];
            int nw = 0;
            while (nw < MAX_ARGS) {                      // découpe en mots
                while (isspace((unsigned char)*p)) p++;
                if (!*p || *p == '<' || *p == '>') break;
                words[nw++] = parse_word(&p);
            }
            if (nw < 2 || strcmp(words[1], "in")) {      // forme attendue : for NOM in ...
                for (int k = 0; k < nw; k++) free(words[k]);
                free(e);
                return syntax_error(s);
            }
            for (int k = 2; k < nw && !exit_requested && !interrupted; k++) {   // Ctrl+C arrête la boucle
                var_set(words[0], words[k], 0);          // x = valeur courante
                status = exec_range(st, i + 2, done);    // exécute le corps
            }
            for (int k = 0; k < nw; k++) free(words[k]);
            free(e);
            i = done + 1;
        } else if (is_closer(s) || !strcmp(s, "then") || !strcmp(s, "else") || !strcmp(s, "do")) {
            return syntax_error(s);                      // mot-clé orphelin
        } else {
            status = exec_statement(s);                  // instruction ordinaire
            i++;
        }
        last_status = status;   // $? = code de la dernière commande/bloc (pas celui d'une condition qui a échoué)
    }
    last_status = status;   
    return status;
}

/* Exécute un texte entier (script, ligne, contenu de $( )). */
static int run_source(const char *src)
{
    Stmts st = {0};
    split_statements(src, &st, NULL);
    int status = exec_range(&st, 0, st.n);
    stmts_free(&st);
    return status;
}

/* Vrai si le texte est incomplet (guillemet ou bloc if/for/while pas fermé) :
 * dans ce cas le REPL lit une ligne de plus (prompt "...> "). */
static int is_incomplete(const char *src)
{
    Stmts st = {0};
    int unterminated = 0, depth = 0;
    split_statements(src, &st, &unterminated);
    for (int i = 0; i < st.n; i++) {
        if (is_opener(st.v[i])) depth++;
        else if (is_closer(st.v[i])) depth--;
    }
    stmts_free(&st);
    return unterminated || depth > 0;
}

/* ---------- 2.G  Fichiers et boucle principale (jalons 1 et 5) ---------- */

/* Lit tout un flux (fichier ou stdin) en mémoire. */
static char *read_stream(FILE *f)
{
    Buf b = {0};
    char tmp[4096];
    size_t r;
    while ((r = fread(tmp, 1, sizeof tmp, f)) > 0)
        buf_addn(&b, tmp, r);
    return buf_take(&b);
}

/* Lit ~/.ocshrc et l'exécute comme un petit script (s'il existe). */
static void load_rc(void)
{
    char *path = home_file(".ocshrc");
    FILE *f = path ? fopen(path, "r") : NULL;
    if (f) {
        char *src = read_stream(f);
        fclose(f);
        run_source(src);
        free(src);
    }
    free(path);
}

/*  LE REPL : Read -> Eval -> Print -> Loop.
 *   lecture d'une ligne -> (si incomplète : on relit) -> historique -> exécution. */
static void interactive_loop(void)
{
    history_load();
    while (!exit_requested) {
        print_done_notices();                            // annonce les tâches de fond finies
        interrupted = 0;
        Buf src = {0};                                   // la commande en cours (peut tenir sur plusieurs lignes)
        for (;;) {
            char *line = read_line(src.len ? "...> " : "ocsh> ");
            if (!line) {
                if (interrupted) {                       // Ctrl+C : on abandonne la saisie
                    interrupted = 0;
                    last_status = 130;
                    free(src.s);
                    break;                               // retour au prompt
                }
                free(src.s);                             // Ctrl+D : fin du shell
                exit_requested = 1;
                exit_code = last_status;
                break;
            }
            buf_add(&src, line);
            buf_addc(&src, '\n');
            free(line);
            if (is_incomplete(src.s))
                continue;                                // il manque la suite : on lit une ligne de plus
            while (src.len && src.s[src.len - 1] == '\n') src.s[--src.len] = '\0';
            /* historique : une commande sur plusieurs lignes est stockée sur une seule */
            char *one = xstrdup(src.s);
            for (char *p = one; *p; p++)
                if (*p == '\n') *p = ';';
            history_add(one);
            free(one);
            run_source(src.s);                           // EXÉCUTION
            free(src.s);
            break;
        }
    }
    history_save();
}

/* Programme principal : choisit le mode (script, -c, interactif, stdin). */
int main(int argc, char **argv)
{
    signals_setup();                                     // Ctrl+C, SIGCHLD...
    char *src = NULL;
    if (argc >= 3 && !strcmp(argv[1], "-c")) {           // ocsh -c 'commande'
        src = xstrdup(argv[2]);
    } else if (argc >= 2) {                              // ocsh script.ocsh  (mode script)
        FILE *f = fopen(argv[1], "r");
        if (!f) { perror(argv[1]); return 127; }
        src = read_stream(f);
        fclose(f);
    } else if (isatty(STDIN_FILENO)) {                   // clavier : mode interactif
        interactive = 1;
        load_rc();                                       // ~/.ocshrc
        if (!exit_requested) interactive_loop();
    } else {                                             // stdin redirigé : echo "cmd" | ./ocsh
        src = read_stream(stdin);
    }
    if (src) {                                           // modes non interactifs : tout d'un coup
        run_source(src);
        free(src);
    }
    return exit_requested ? exit_code : last_status;
}