/* exec.c - fork / pipe / dup2 / execvp with process groups and job control.
 *
 * Every command line becomes a job: one process per command, all in one
 * process group. A foreground job is given the terminal with tcsetpgrp(),
 * so the kernel delivers Ctrl-C (SIGINT) and Ctrl-Z (SIGTSTP) to the job
 * and not to the shell.
 */
#include "exec.h"
#include "builtins.h"
#include "shell.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *const k_external[] = {"mkdir", "cat", "mv", "ls", "ps", "gcc", NULL};

int command_allowed(const char *name) {
    if (is_builtin(name) || is_internal_program(name)) return 1;
    if (strcmp(name, "ps-aux") == 0) return 1;
    if (strchr(name, '/')) return 1;          /* ./a.out, ./prog, /path/to/prog */
    for (int i = 0; k_external[i]; i++)
        if (strcmp(name, k_external[i]) == 0) return 1;
    return 0;
}

static int apply_redirects(const command *cmd) {
    for (int i = 0; i < cmd->nredirs; i++) {
        const redirect *r = &cmd->redirs[i];
        if (r->kind == R_DUP) {
            if (dup2(r->src_fd, r->fd) < 0) {
                fprintf(stderr, "minishell: %d: %s\n", r->src_fd, strerror(errno));
                return -1;
            }
            continue;
        }
        int flags = r->kind == R_READ   ? O_RDONLY
                  : r->kind == R_APPEND ? O_WRONLY | O_CREAT | O_APPEND
                                        : O_WRONLY | O_CREAT | O_TRUNC;
        int fd = open(r->target, flags, 0644);
        if (fd < 0) {
            fprintf(stderr, "minishell: %s: %s\n", r->target, strerror(errno));
            return -1;
        }
        if (fd != r->fd) {
            dup2(fd, r->fd);
            close(fd);
        }
    }
    return 0;
}

/* A lone foreground built-in runs inside the shell (cd must change the
 * shell's own directory). Redirected descriptors are saved and restored. */
static int run_builtin_in_shell(const command *cmd) {
    int saved[10];
    for (int i = 0; i < 10; i++) saved[i] = -2;
    for (int i = 0; i < cmd->nredirs; i++) {
        int fd = cmd->redirs[i].fd;
        if (fd >= 0 && fd < 10 && saved[fd] == -2) saved[fd] = fcntl(fd, F_DUPFD_CLOEXEC, 10);
    }
    fflush(NULL);
    int status = apply_redirects(cmd) == 0 ? run_builtin(cmd->argc, cmd->argv) : 1;
    fflush(NULL);
    for (int fd = 0; fd < 10; fd++) {
        if (saved[fd] == -2) continue;
        if (saved[fd] >= 0) {
            dup2(saved[fd], fd);
            close(saved[fd]);
        } else {
            close(fd);
        }
    }
    return status;
}

static _Noreturn void run_child(const command *cmd) {
    if (apply_redirects(cmd) < 0) _exit(1);

    if (is_builtin(cmd->argv[0])) {
        int status = run_builtin(cmd->argc, cmd->argv);
        fflush(NULL);
        _exit(status);
    }
    if (is_internal_program(cmd->argv[0])) run_internal_program(cmd->argc, cmd->argv);

    /* `ps -aux` is Linux syntax; BSD/macOS ps wants `ps aux`. `ps-aux` is accepted as a shortcut. */
#ifdef __APPLE__
    const char *aux = "aux";
#else
    const char *aux = "-aux";
#endif
    if (strcmp(cmd->argv[0], "ps-aux") == 0) {
        char *args[] = {"ps", (char *)aux, NULL};
        execvp("ps", args);
    } else {
        if (strcmp(cmd->argv[0], "ps") == 0)
            for (int i = 1; i < cmd->argc; i++)
                if (strcmp(cmd->argv[i], "-aux") == 0) cmd->argv[i] = (char *)aux;
        execvp(cmd->argv[0], cmd->argv);
    }

    int e = errno;
    if (e == ENOENT && !strchr(cmd->argv[0], '/')) {
        fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
        _exit(127);
    }
    fprintf(stderr, "minishell: %s: %s\n", cmd->argv[0], strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

int execute(const pipeline *pl) {
    for (int i = 0; i < pl->ncmds; i++) {
        if (!command_allowed(pl->cmds[i].argv[0])) {
            fprintf(stderr, "minishell: %s: command not supported (type 'help' to see the available commands)\n",
                    pl->cmds[i].argv[0]);
            return 127;
        }
    }

    if (pl->ncmds == 1 && !pl->background && is_builtin(pl->cmds[0].argv[0]))
        return run_builtin_in_shell(&pl->cmds[0]);

    pid_t pgid = 0;
    pid_t *pids = calloc((size_t)pl->ncmds, sizeof *pids);
    int npids = 0;
    int in_fd = STDIN_FILENO;

    for (int i = 0; i < pl->ncmds; i++) {
        int p[2] = {-1, -1};
        if (i + 1 < pl->ncmds && pipe(p) < 0) {
            perror("minishell: pipe");
            break;
        }
        fflush(NULL);
        pid_t pid = fork();
        if (pid < 0) {
            perror("minishell: fork");
            if (p[0] >= 0) { close(p[0]); close(p[1]); }
            break;
        }

        if (pid == 0) {
            /* ---------- child ---------- */
            if (sh.interactive) {
                setpgid(0, pgid);
                if (!pl->background) tcsetpgrp(sh.tty, getpgrp());
            }
            reset_child_signals();
            if (in_fd != STDIN_FILENO) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (p[1] >= 0) {
                dup2(p[1], STDOUT_FILENO);
                close(p[1]);
                close(p[0]);
            }
            run_child(&pl->cmds[i]);
        }

        /* ---------- parent ---------- */
        if (sh.interactive) {
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);
        }
        pids[npids++] = pid;
        if (in_fd != STDIN_FILENO) close(in_fd);
        in_fd = STDIN_FILENO;
        if (p[1] >= 0) {
            close(p[1]);
            in_fd = p[0];
        }
    }
    if (in_fd != STDIN_FILENO) close(in_fd);

    if (npids == 0) {
        free(pids);
        return 1;
    }
    if (pgid == 0) pgid = pids[0];
    job *j = job_add(pgid, pids, npids, pl->text, pl->background);
    int last_pid = pids[npids - 1];
    free(pids);

    if (pl->background) {
        if (sh.interactive) printf("[%d] %d\n", j->id, last_pid);
        return 0;
    }
    return wait_foreground(j, 0);
}

int wait_foreground(job *j, int resume) {
    if (sh.interactive) {
        tcsetpgrp(sh.tty, j->pgid);
        if (resume) tcsetattr(sh.tty, TCSADRAIN, &j->tmodes);
    }
    if (resume) {
        job_mark_running(j);
        kill(-j->pgid, SIGCONT);
    }
    j->background = 0;

    while (job_state_of(j) == JOB_RUNNING) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) {
                if (g_sighup) break;
                continue;
            }
            for (int i = 0; i < j->nprocs; i++) j->procs[i].completed = 1;
            break;
        }
        jobs_update(pid, status);
    }

    if (sh.interactive) {
        tcsetpgrp(sh.tty, sh.pgid);
        tcgetattr(sh.tty, &j->tmodes);
        tcsetattr(sh.tty, TCSADRAIN, &sh.tmodes);
    }

    int status = job_exit_status(j);
    if (job_state_of(j) == JOB_STOPPED) {
        printf("\n[%d]+  Stopped                 %s\n", j->id, j->command);
        j->notified = 1;
        fflush(stdout);
        return status;
    }
    if (job_state_of(j) == JOB_DONE) {
        int raw = j->procs[j->nprocs - 1].status;
        if (WIFSIGNALED(raw)) {
            int sig = WTERMSIG(raw);
            if (sig == SIGINT) printf("\n");
            else if (sig != SIGPIPE) fprintf(stderr, "%s\n", strsignal(sig));
        }
        job_remove(j);
    }
    fflush(stdout);
    return status;
}
