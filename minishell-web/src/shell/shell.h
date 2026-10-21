/* shell.h - global state of the MiniShell++ terminal shell. */
#ifndef SHELL_H
#define SHELL_H

#include <signal.h>
#include <sys/types.h>
#include <termios.h>

#include "parser.h"
#include "sbuf.h"

typedef struct {
    int interactive;          /* attached to a terminal: job control on */
    int web;                  /* running inside the browser terminal */
    int tty;                  /* terminal file descriptor (stdin) */
    pid_t pgid;               /* the shell's own process group */
    struct termios tmodes;    /* normal (cooked) terminal settings */
    int last_status;          /* $? */
    int running;
    int exit_code;
    int warned_stopped;
    strlist history;
} shell_t;

extern shell_t sh;

extern volatile sig_atomic_t g_sigint;
extern volatile sig_atomic_t g_sighup;

void shell_init(int interactive);
void shell_run_line(const char *line);
void shell_shutdown(void);
char *shell_prompt(void);                 /* caller frees */
void reset_child_signals(void);

/* Line editor with history (lineedit.c).
 * Returns 1 = got a line (*out allocated), 0 = EOF, -1 = cancelled with Ctrl-C. */
int read_command(const char *prompt, char **out);

#endif
