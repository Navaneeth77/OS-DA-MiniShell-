/* parser.h - turns a command line into a pipeline of commands. */
#ifndef PARSER_H
#define PARSER_H

#include <stddef.h>

typedef enum { R_READ, R_WRITE, R_APPEND, R_DUP } redirect_kind;

typedef struct {
    redirect_kind kind;
    int fd;          /* descriptor being redirected: 0, 1, 2 ... */
    int src_fd;      /* R_DUP only: 2>&1 -> fd 2, src_fd 1 */
    char *target;    /* file name for R_READ / R_WRITE / R_APPEND */
} redirect;

typedef struct {
    char **argv;     /* NULL-terminated, ready for execvp() */
    int argc;
    int argv_cap;
    redirect *redirs;
    int nredirs;
    int redirs_cap;
} command;

typedef struct {
    command *cmds;
    int ncmds;
    int cmds_cap;
    int background;  /* trailing & */
    char *text;      /* trimmed command text, used for job listings */
} pipeline;

/* Returns 0 on success, -1 on a syntax error (message written to err).
 * last_status is substituted for $?. A blank/comment line gives ncmds == 0. */
int parse_line(const char *line, int last_status, pipeline *out, char *err, size_t errlen);
void pipeline_free(pipeline *pl);

#endif
