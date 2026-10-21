/* jobs.c - job table, status tracking and notifications. */
#include "jobs.h"
#include "sbuf.h"
#include "shell.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static job *g_jobs;   /* linked list ordered by id */

job *job_add(pid_t pgid, const pid_t *pids, int n, const char *command, int background) {
    job *j = calloc(1, sizeof *j);
    if (!j) { perror("calloc"); exit(1); }
    j->pgid = pgid;
    j->command = xstrdup(command);
    j->background = background;
    j->tmodes = sh.tmodes;
    j->nprocs = n;
    j->procs = calloc((size_t)n, sizeof *j->procs);
    for (int i = 0; i < n; i++) j->procs[i].pid = pids[i];

    int id = 1;
    job **tail = &g_jobs;
    while (*tail) {
        if ((*tail)->id >= id) id = (*tail)->id + 1;
        tail = &(*tail)->next;
    }
    j->id = id;
    *tail = j;
    return j;
}

job *job_first(void) { return g_jobs; }

job *job_find(int id) {
    for (job *j = g_jobs; j; j = j->next)
        if (j->id == id) return j;
    return NULL;
}

job *job_current(void) {
    job *last = NULL;
    for (job *j = g_jobs; j; j = j->next) last = j;
    return last;
}

job *job_previous(void) {
    job *prev = NULL;
    for (job *j = g_jobs; j && j->next; j = j->next) prev = j;
    return prev;
}

void job_remove(job *j) {
    for (job **p = &g_jobs; *p; p = &(*p)->next) {
        if (*p != j) continue;
        *p = j->next;
        free(j->command);
        free(j->procs);
        free(j);
        return;
    }
}

job_state job_state_of(const job *j) {
    int all_done = 1, any_running = 0;
    for (int i = 0; i < j->nprocs; i++) {
        if (j->procs[i].completed) continue;
        all_done = 0;
        if (!j->procs[i].stopped) any_running = 1;
    }
    if (all_done) return JOB_DONE;
    return any_running ? JOB_RUNNING : JOB_STOPPED;
}

int job_exit_status(const job *j) {
    int st = j->procs[j->nprocs - 1].status;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    if (WIFSTOPPED(st)) return 128 + WSTOPSIG(st);
    return 0;
}

void job_mark_running(job *j) {
    for (int i = 0; i < j->nprocs; i++) j->procs[i].stopped = 0;
    j->notified = 0;
}

void jobs_update(pid_t pid, int status) {
    for (job *j = g_jobs; j; j = j->next) {
        for (int i = 0; i < j->nprocs; i++) {
            process *p = &j->procs[i];
            if (p->pid != pid) continue;
            if (WIFSTOPPED(status)) {
                p->stopped = 1;
                p->status = status;
            } else if (WIFCONTINUED(status)) {
                p->stopped = 0;
            } else {
                p->completed = 1;
                p->stopped = 0;
                p->status = status;
            }
            j->notified = 0;
            return;
        }
    }
}

void jobs_reap(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        jobs_update(pid, status);
}

static void describe(const job *j, char *buf, size_t len) {
    switch (job_state_of(j)) {
    case JOB_RUNNING: snprintf(buf, len, "Running"); return;
    case JOB_STOPPED: snprintf(buf, len, "Stopped"); return;
    case JOB_DONE: break;
    }
    int st = j->procs[j->nprocs - 1].status;
    if (WIFSIGNALED(st)) snprintf(buf, len, "%s", strsignal(WTERMSIG(st)));
    else if (WEXITSTATUS(st)) snprintf(buf, len, "Exit %d", WEXITSTATUS(st));
    else snprintf(buf, len, "Done");
}

static void print_job(FILE *out, const job *j, int long_format) {
    char state[64];
    describe(j, state, sizeof state);
    char mark = j == job_current() ? '+' : j == job_previous() ? '-' : ' ';
    fprintf(out, "[%d]%c  ", j->id, mark);
    if (long_format) fprintf(out, "%d ", (int)j->pgid);
    fprintf(out, "%-24s%s%s\n", state, j->command,
            job_state_of(j) == JOB_RUNNING && j->background ? " &" : "");
}

void jobs_notify(void) {
    job *j = g_jobs;
    while (j) {
        job *next = j->next;
        job_state s = job_state_of(j);
        if (s == JOB_DONE) {
            if (sh.interactive && !j->notified) print_job(stdout, j, 0);
            job_remove(j);
        } else if (s == JOB_STOPPED && !j->notified) {
            if (sh.interactive) print_job(stdout, j, 0);
            j->notified = 1;
        }
        j = next;
    }
    fflush(stdout);
}

void jobs_print(FILE *out, int long_format) {
    for (job *j = g_jobs; j; j = j->next) {
        print_job(out, j, long_format);
        if (job_state_of(j) != JOB_RUNNING) j->notified = 1;
    }
    /* finished jobs have now been reported: drop them */
    job *j = g_jobs;
    while (j) {
        job *next = j->next;
        if (job_state_of(j) == JOB_DONE) job_remove(j);
        j = next;
    }
}

int jobs_have_stopped(void) {
    for (job *j = g_jobs; j; j = j->next)
        if (job_state_of(j) == JOB_STOPPED) return 1;
    return 0;
}
