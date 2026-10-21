/* jobs.h - job table for job control (Ctrl-Z, jobs, fg, bg). */
#ifndef JOBS_H
#define JOBS_H

#include <stdio.h>
#include <sys/types.h>
#include <termios.h>

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_state;

typedef struct {
    pid_t pid;
    int completed;
    int stopped;
    int status;               /* raw waitpid() status */
} process;

typedef struct job {
    int id;                   /* [1], [2] ... */
    pid_t pgid;
    char *command;
    process *procs;
    int nprocs;
    int background;
    int notified;
    struct termios tmodes;    /* terminal settings saved when the job stopped */
    struct job *next;
} job;

job *job_add(pid_t pgid, const pid_t *pids, int n, const char *command, int background);
job *job_find(int id);
job *job_current(void);       /* %+ */
job *job_previous(void);      /* %- */
job *job_first(void);
void job_remove(job *j);

job_state job_state_of(const job *j);
int job_exit_status(const job *j);
void job_mark_running(job *j);

void jobs_update(pid_t pid, int status);
void jobs_reap(void);
void jobs_notify(void);                    /* print Done/Stopped changes */
void jobs_print(FILE *out, int long_format);
int jobs_have_stopped(void);

#endif
