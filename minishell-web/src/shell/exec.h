/* exec.h - running pipelines with fork/exec and terminal job control. */
#ifndef EXEC_H
#define EXEC_H

#include "jobs.h"
#include "parser.h"

/* Runs a parsed command line and returns its exit status. */
int execute(const pipeline *pl);

/* Gives the terminal to the job, optionally continues it (fg), and waits
 * until it exits or stops. Returns the job's exit status. */
int wait_foreground(job *j, int resume);

/* Is this one of the commands MiniShell++ supports? */
int command_allowed(const char *name);

#endif
