/* main.c - MiniShell++ entry point.
 *
 *   ./minishell              interactive shell (works in any terminal)
 *   ./minishell -c "cmd"     run one command line
 */
#include "jobs.h"
#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        shell_init(0);
        shell_run_line(argv[2]);
        return sh.running ? sh.last_status : sh.exit_code;
    }

    int interactive = isatty(STDIN_FILENO);
    shell_init(interactive);

    while (sh.running && !g_sighup) {
        jobs_reap();
        jobs_notify();

        char *prompt = interactive ? shell_prompt() : xstrdup("");
        char *line = NULL;
        int r = read_command(prompt, &line);
        free(prompt);

        if (g_sighup) break;
        if (r == 0) {   /* Ctrl-D */
            if (interactive && jobs_have_stopped() && !sh.warned_stopped) {
                printf("\nThere are stopped jobs.\n");
                sh.warned_stopped = 1;
                continue;
            }
            if (interactive) printf("exit\n");
            sh.exit_code = sh.last_status;
            break;
        }
        if (r < 0) continue;   /* Ctrl-C at the prompt */

        shell_run_line(line);
        free(line);
    }

    shell_shutdown();
    return sh.exit_code;
}
