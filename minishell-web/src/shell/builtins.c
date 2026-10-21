/* builtins.c - cd, pwd, pid, jobs, fg, bg, kill, exit, clear, help
 * and the internal programs gedit and man. */
#include "builtins.h"
#include "exec.h"
#include "jobs.h"
#include "shell.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fail(const char *name, const char *msg) {
    fprintf(stderr, "minishell: %s: %s\n", name, msg);
    return 1;
}

static int parse_long(const char *s, long *v) {
    if (!*s) return 0;
    char *end;
    errno = 0;
    *v = strtol(s, &end, 10);
    return errno == 0 && *end == '\0';
}

/* %n, n, %%, %+, %- or nothing */
static job *resolve_job(int argc, char **argv, const char *name) {
    job *j;
    if (argc < 2 || !strcmp(argv[1], "%%") || !strcmp(argv[1], "%+")) {
        if (!(j = job_current())) fail(name, "current: no such job");
        return j;
    }
    if (!strcmp(argv[1], "%-")) {
        if (!(j = job_previous())) fail(name, "previous: no such job");
        return j;
    }
    long id;
    j = parse_long(argv[1][0] == '%' ? argv[1] + 1 : argv[1], &id) ? job_find((int)id) : NULL;
    if (!j) fprintf(stderr, "minishell: %s: %s: no such job\n", name, argv[1]);
    return j;
}

/* ------------------------------------------------------------ built-ins */

static int bi_cd(int argc, char **argv) {
    char target[PATH_MAX], old[PATH_MAX], cwd[PATH_MAX];
    int print = 0;
    if (argc > 2) return fail("cd", "too many arguments");
    if (argc == 1) {
        const char *home = getenv("HOME");
        if (!home) return fail("cd", "HOME not set");
        snprintf(target, sizeof target, "%s", home);
    } else if (!strcmp(argv[1], "-")) {
        const char *o = getenv("OLDPWD");
        if (!o) return fail("cd", "OLDPWD not set");
        snprintf(target, sizeof target, "%s", o);
        print = 1;
    } else {
        snprintf(target, sizeof target, "%s", argv[1]);
    }
    int have_old = getcwd(old, sizeof old) != NULL;
    if (chdir(target) < 0) {
        fprintf(stderr, "minishell: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    if (have_old) setenv("OLDPWD", old, 1);
    if (getcwd(cwd, sizeof cwd)) {
        setenv("PWD", cwd, 1);
        if (print) printf("%s\n", cwd);
    }
    return 0;
}

static int bi_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return fail("pwd", strerror(errno));
    printf("%s\n", cwd);
    return 0;
}

static int bi_pid(int argc, char **argv) {
    if (argc == 1) {
        printf("PID  (this shell)     %d\n", (int)getpid());
        printf("PPID (parent)         %d\n", (int)getppid());
        printf("PGID (process group)  %d\n", (int)getpgrp());
        printf("SID  (session)        %d\n", (int)getsid(0));
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '%') {
            long id;
            job *j = parse_long(argv[i] + 1, &id) ? job_find((int)id) : NULL;
            if (!j) { fprintf(stderr, "minishell: pid: %s: no such job\n", argv[i]); status = 1; continue; }
            printf("[%d] %s   (process group %d)\n", j->id, j->command, (int)j->pgid);
            for (int k = 0; k < j->nprocs; k++)
                printf("    PID %d  %s\n", (int)j->procs[k].pid,
                       j->procs[k].completed ? "finished" : j->procs[k].stopped ? "stopped" : "running");
            continue;
        }
        /* pid NAME: like pidof, find processes by command name */
        FILE *ps = popen("ps -axo pid=,comm=", "r");
        if (!ps) return fail("pid", strerror(errno));
        char line[1024];
        int found = 0;
        while (fgets(line, sizeof line, ps)) {
            int p;
            char comm[900];
            if (sscanf(line, "%d %899[^\n]", &p, comm) != 2) continue;
            char *base = strrchr(comm, '/');
            base = base ? base + 1 : comm;
            if (strcmp(base, argv[i]) == 0) {
                printf("%s%d", found ? " " : "", p);
                found = 1;
            }
        }
        pclose(ps);
        if (found) printf("\n");
        else { fprintf(stderr, "pid: no process named '%s'\n", argv[i]); status = 1; }
    }
    return status;
}

static int bi_jobs(int argc, char **argv) {
    jobs_reap();
    jobs_print(stdout, argc > 1 && !strcmp(argv[1], "-l"));
    return 0;
}

static int bi_fg(int argc, char **argv) {
    if (!sh.interactive || getpid() != sh.pgid) return fail("fg", "no job control");
    jobs_reap();
    job *j = resolve_job(argc, argv, "fg");
    if (!j) return 1;
    if (job_state_of(j) == JOB_DONE) return fail("fg", "job has terminated");
    printf("%s\n", j->command);
    fflush(stdout);
    return wait_foreground(j, 1);
}

static int bi_bg(int argc, char **argv) {
    if (!sh.interactive || getpid() != sh.pgid) return fail("bg", "no job control");
    jobs_reap();
    job *j = resolve_job(argc, argv, "bg");
    if (!j) return 1;
    if (job_state_of(j) == JOB_DONE) return fail("bg", "job has terminated");
    if (job_state_of(j) == JOB_RUNNING) {
        fprintf(stderr, "minishell: bg: job %d already in background\n", j->id);
        return 0;
    }
    job_mark_running(j);
    j->background = 1;
    kill(-j->pgid, SIGCONT);
    printf("[%d]+ %s &\n", j->id, j->command);
    return 0;
}

static const struct { const char *name; int sig; } k_signals[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"KILL", SIGKILL}, {"TERM", SIGTERM},
    {"CONT", SIGCONT}, {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
};

static int signal_from(const char *s) {
    long n;
    if (parse_long(s, &n)) return n > 0 && n < 32 ? (int)n : -1;
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (size_t i = 0; i < sizeof k_signals / sizeof *k_signals; i++)
        if (!strcmp(s, k_signals[i].name)) return k_signals[i].sig;
    return -1;
}

static int bi_kill(int argc, char **argv) {
    if (argc < 2) return fail("kill", "usage: kill [-SIGNAL] pid | %job ...  (kill -l lists signals)");
    if (!strcmp(argv[1], "-l")) {
        for (size_t i = 0; i < sizeof k_signals / sizeof *k_signals; i++)
            printf("%2d) SIG%s\n", k_signals[i].sig, k_signals[i].name);
        return 0;
    }
    int sig = SIGTERM, i = 1;
    if (argv[1][0] == '-') {
        sig = signal_from(argv[1] + 1);
        i = 2;
        if (sig < 0) return fail("kill", "invalid signal specification");
    }
    if (i >= argc) return fail("kill", "usage: kill [-SIGNAL] pid | %job ...");
    int status = 0;
    for (; i < argc; i++) {
        if (argv[i][0] == '%') {
            long id;
            job *j = parse_long(argv[i] + 1, &id) ? job_find((int)id) : NULL;
            if (!j) { fprintf(stderr, "minishell: kill: %s: no such job\n", argv[i]); status = 1; continue; }
            int stopped = job_state_of(j) == JOB_STOPPED;
            if (kill(-j->pgid, sig) < 0) { fprintf(stderr, "minishell: kill: %s\n", strerror(errno)); status = 1; continue; }
            if (stopped && sig != SIGSTOP && sig != SIGTSTP && sig != SIGCONT) kill(-j->pgid, SIGCONT);
        } else {
            long pid;
            if (!parse_long(argv[i], &pid)) {
                fprintf(stderr, "minishell: kill: %s: arguments must be process or job IDs\n", argv[i]);
                status = 1;
            } else if (kill((pid_t)pid, sig) < 0) {
                fprintf(stderr, "minishell: kill: (%ld) - %s\n", pid, strerror(errno));
                status = 1;
            }
        }
    }
    return status;
}

static int bi_exit(int argc, char **argv) {
    if (sh.interactive && jobs_have_stopped() && !sh.warned_stopped) {
        fprintf(stderr, "There are stopped jobs.\n");
        sh.warned_stopped = 1;
        return 1;
    }
    long code = sh.last_status;
    if (argc > 1 && !parse_long(argv[1], &code)) {
        fail("exit", "numeric argument required");
        code = 2;
    }
    if (sh.interactive) fprintf(stderr, "exit\n");
    sh.exit_code = (int)(code & 0xff);
    sh.running = 0;
    return sh.exit_code;
}

static int bi_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("\033[H\033[2J\033[3J");
    return 0;
}

static int bi_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("MiniShell++ - supported commands\n\n"
           "  Files & directories\n"
           "    pwd                    print the current directory\n"
           "    cd [dir]               change directory (cd, cd .., cd -)\n"
           "    ls [-l] [-a] [dir]     list files\n"
           "    mkdir [-p] dir         create a directory\n"
           "    mv src dest            move or rename\n"
           "    cat file               show a file\n"
           "    cat >> file            append typed text to a file (end with Ctrl-D)\n"
           "\n"
           "  Programs\n"
           "    gedit file.c           edit a file in the text editor\n"
           "    gcc file.c [-o name]   compile a C program\n"
           "    ./a.out                run the compiled program\n"
           "    man command            show the manual page\n"
           "\n"
           "  Processes\n"
           "    ps / ps -aux           list processes\n"
           "    pid [name | %%job]      show process IDs\n"
           "\n"
           "  Job control (Ctrl-Z)\n"
           "    Ctrl-C                 interrupt the running program (SIGINT)\n"
           "    Ctrl-Z                 stop the running program (SIGTSTP)\n"
           "    jobs [-l]              list stopped and background jobs\n"
           "    fg [%%n]                continue a job in the foreground\n"
           "    bg [%%n]                continue a job in the background\n"
           "    kill [-SIG] %%n | pid   send a signal\n"
           "    command &              start in the background\n"
           "\n"
           "  Also: clear, exit, help, > >> < redirection, | pipes\n");
    return 0;
}

static const struct { const char *name; int (*fn)(int, char **); } k_builtins[] = {
    {"cd", bi_cd}, {"pwd", bi_pwd}, {"pid", bi_pid}, {"jobs", bi_jobs}, {"fg", bi_fg},
    {"bg", bi_bg}, {"kill", bi_kill}, {"exit", bi_exit}, {"clear", bi_clear}, {"help", bi_help},
};
#define NUM_BUILTINS (int)(sizeof k_builtins / sizeof *k_builtins)

int is_builtin(const char *name) {
    for (int i = 0; i < NUM_BUILTINS; i++)
        if (!strcmp(name, k_builtins[i].name)) return 1;
    return 0;
}

int run_builtin(int argc, char **argv) {
    for (int i = 0; i < NUM_BUILTINS; i++)
        if (!strcmp(argv[0], k_builtins[i].name)) return k_builtins[i].fn(argc, argv);
    return 127;
}

/* ------------------------------------------------------ internal programs */

typedef struct {
    const char *name;
    const char *summary;
    const char *synopsis;
    const char *description;
} man_page;

static const man_page k_pages[] = {
    {"cd", "change the working directory", "cd [dir]\n       cd -\n       cd ..",
     "Changes the current directory of the shell. With no argument it goes to $HOME;\n"
     "       `cd -` returns to the previous directory. cd is a shell built-in: it must run\n"
     "       inside the shell process, because a child process cannot change its parent's\n"
     "       working directory (chdir(2) only affects the calling process)."},
    {"pid", "show process IDs", "pid\n       pid NAME\n       pid %JOB",
     "With no argument, prints the PID, parent PID, process group and session of the\n"
     "       shell (getpid(2), getppid(2), getpgrp(2), getsid(2)).\n"
     "       pid NAME prints the PIDs of running processes called NAME, like pidof(1).\n"
     "       pid %JOB prints every process of a job and whether it is running or stopped."},
    {"jobs", "list jobs", "jobs [-l]",
     "Lists background and stopped jobs. + marks the current job, - the previous one.\n"
     "       -l also prints each job's process group ID."},
    {"fg", "resume a job in the foreground", "fg [%JOB]",
     "Gives the terminal to the job with tcsetpgrp(3), sends it SIGCONT and waits for it,\n"
     "       exactly as if it had been started in the foreground."},
    {"bg", "resume a stopped job in the background", "bg [%JOB]",
     "Sends SIGCONT to a job stopped with Ctrl-Z so it keeps running while the shell\n"
     "       accepts new commands."},
    {"kill", "send a signal to a process or job", "kill [-SIGNAL] PID | %JOB ...\n       kill -l",
     "Sends SIGTERM (or the given signal) with kill(2). %JOB sends it to the job's whole\n"
     "       process group. A stopped job is also sent SIGCONT so it can act on the signal."},
    {"exit", "leave the shell", "exit [N]", "Exits the shell with status N (default: the last status)."},
    {"clear", "clear the terminal screen", "clear", "Clears the screen and scrollback."},
    {"help", "list MiniShell++ commands", "help", "Prints every command MiniShell++ supports."},
    {"gedit", "text editor", "gedit FILE",
     "Opens FILE in the graphical text editor. The terminal waits until the editor\n"
     "       window is closed; start it with `gedit FILE &` to keep using the terminal.\n"
     "       Save with Ctrl+S. Pressing Ctrl-Z in the terminal stops the editor process\n"
     "       (the window stops responding until `fg` or `bg`)."},
};

static const man_page *find_page(const char *name) {
    for (size_t i = 0; i < sizeof k_pages / sizeof *k_pages; i++) {
        if (strcmp(k_pages[i].name, name) != 0) continue;
        if (!strcmp(name, "gedit") && !sh.web) return NULL;   /* real gedit has its own page */
        return &k_pages[i];
    }
    return NULL;
}

static _Noreturn void man_program(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "What manual page do you want?\nFor example, try 'man man'.\n");
        _exit(1);
    }
    const man_page *page = find_page(argv[argc - 1]);
    if (page) {
        char upper[64];
        size_t n = 0;
        for (; page->name[n] && n < sizeof upper - 1; n++) upper[n] = (char)toupper((unsigned char)page->name[n]);
        upper[n] = '\0';
        printf("%s(1)                      MiniShell++ Manual                      %s(1)\n\n", upper, upper);
        printf("\033[1mNAME\033[0m\n       %s - %s\n\n", page->name, page->summary);
        printf("\033[1mSYNOPSIS\033[0m\n       %s\n\n", page->synopsis);
        printf("\033[1mDESCRIPTION\033[0m\n       %s\n\n", page->description);
        printf("MiniShell++ 1.0                  September 2026                  %s(1)\n", upper);
        fflush(stdout);
        _exit(0);
    }
    /* Real manual page, printed straight to the terminal (no pager). */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        char cols[16];
        snprintf(cols, sizeof cols, "%d", ws.ws_col > 2 ? ws.ws_col - 2 : ws.ws_col);
        setenv("MANWIDTH", cols, 1);
    }
    setenv("MANPAGER", "cat", 1);
    setenv("PAGER", "cat", 1);
    execvp("man", argv);
    fprintf(stderr, "minishell: man: %s\n", strerror(errno));
    _exit(127);
}

static volatile sig_atomic_t g_editor_closed;
static void on_editor_close(int sig) { (void)sig; g_editor_closed = 1; }

static _Noreturn void gedit_program(int argc, char **argv) {
    if (!sh.web) {
        execvp("gedit", argv);
        fprintf(stderr, "gedit: not installed on this system (the MiniShell++ web terminal has a built-in gedit)\n");
        _exit(127);
    }
    if (argc < 2) {
        fprintf(stderr, "gedit: please give a file name, for example: gedit h.c\n");
        _exit(1);
    }

    char path[PATH_MAX], cwd[PATH_MAX];
    if (argv[1][0] == '/') snprintf(path, sizeof path, "%s", argv[1]);
    else snprintf(path, sizeof path, "%s/%s", getcwd(cwd, sizeof cwd) ? cwd : ".", argv[1]);

    /* The browser opens the editor window when it sees this private escape
     * sequence; closing the window sends this process SIGUSR1. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_editor_close;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);

    printf("\033]7777;gedit;%d;", (int)getpid());
    for (const char *c = path; *c; c++) printf("%02x", (unsigned char)*c);
    printf("\007");
    fflush(stdout);

    while (!g_editor_closed) sigsuspend(&old);
    _exit(0);
}

int is_internal_program(const char *name) { return !strcmp(name, "gedit") || !strcmp(name, "man"); }

void run_internal_program(int argc, char **argv) {
    if (!strcmp(argv[0], "gedit")) gedit_program(argc, argv);
    man_program(argc, argv);
}
