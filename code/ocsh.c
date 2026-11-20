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
