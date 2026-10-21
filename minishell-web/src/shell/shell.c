/* shell.c - initialisation, signals, prompt and running a command line. */
#include "shell.h"
#include "exec.h"
#include "jobs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

shell_t sh;
volatile sig_atomic_t g_sigint;
volatile sig_atomic_t g_sighup;

static void on_sigint(int sig) { (void)sig; g_sigint = 1; }
static void on_sighup(int sig) { (void)sig; g_sighup = 1; }
static void on_sigchld(int sig) { (void)sig; }

static void set_handler(int sig, void (*fn)(int), int flags) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void reset_child_signals(void) {
    int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGHUP, SIGPIPE};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) set_handler(sigs[i], SIG_DFL, 0);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);
}

void shell_init(int interactive) {
    memset(&sh, 0, sizeof sh);
    sh.interactive = interactive;
    sh.web = getenv("MINISHELL_WEB") != NULL;
    sh.tty = STDIN_FILENO;
    sh.running = 1;

    if (interactive) {
        /* Wait until we are in the foreground of the terminal. */
        pid_t pgrp;
        while (tcgetpgrp(sh.tty) != (pgrp = getpgrp())) kill(-pgrp, SIGTTIN);

        set_handler(SIGINT, on_sigint, 0);       /* no SA_RESTART: interrupts read() */
        set_handler(SIGHUP, on_sighup, 0);
        set_handler(SIGQUIT, SIG_IGN, 0);
        set_handler(SIGTSTP, SIG_IGN, 0);        /* Ctrl-Z must not stop the shell */
        set_handler(SIGTTIN, SIG_IGN, 0);
        set_handler(SIGTTOU, SIG_IGN, 0);

        setpgid(0, 0);                            /* own process group (fails harmlessly if session leader) */
        sh.pgid = getpgrp();
        tcsetpgrp(sh.tty, sh.pgid);
        tcgetattr(sh.tty, &sh.tmodes);
    } else {
        sh.pgid = getpgrp();
    }
    set_handler(SIGCHLD, on_sigchld, SA_RESTART);
}

void shell_shutdown(void) {
    /* Like bash: jobs get SIGHUP when the shell exits. */
    for (job *j = job_first(); j; j = j->next) {
        if (job_state_of(j) == JOB_DONE) continue;
        kill(-j->pgid, SIGHUP);
        kill(-j->pgid, SIGCONT);
    }
}

char *shell_prompt(void) {
    char host[256] = "localhost", cwd[PATH_MAX], shown[PATH_MAX];
    gethostname(host, sizeof host);
    char *dot = strchr(host, '.');
    if (dot) *dot = '\0';
    const char *user = getenv("USER");
    if (!user) user = "user";

    if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, "?");
    const char *home = getenv("HOME");
    size_t hl = home ? strlen(home) : 0;
    if (hl > 1 && !strncmp(cwd, home, hl) && (cwd[hl] == '\0' || cwd[hl] == '/'))
        snprintf(shown, sizeof shown, "~%s", cwd + hl);
    else
        snprintf(shown, sizeof shown, "%s", cwd);

    sbuf b;
    sb_init(&b);
    sb_printf(&b, "\033]0;%s@%s: %s\007", user, host, shown);                     /* window title */
    sb_printf(&b, "\033[01;32m%s@%s\033[00m:\033[01;34m%s\033[00m$ ", user, host, shown); /* Ubuntu colours */
    return b.data;
}

void shell_run_line(const char *line) {
    if (strspn(line, " \t") == strlen(line)) return;
    if (!sh.history.count || strcmp(sh.history.items[sh.history.count - 1], line) != 0)
        sl_push(&sh.history, xstrdup(line));

    char err[512];
    pipeline pl;
    if (parse_line(line, sh.last_status, &pl, err, sizeof err) < 0) {
        fprintf(stderr, "minishell: %s\n", err);
        sh.last_status = 2;
        return;
    }
    if (pl.ncmds > 0) {
        if (strcmp(pl.cmds[0].argv[0], "exit") != 0) sh.warned_stopped = 0;
        sh.last_status = execute(&pl);
    }
    pipeline_free(&pl);
    fflush(NULL);
}
