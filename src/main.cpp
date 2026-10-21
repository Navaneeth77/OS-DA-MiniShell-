// main.cpp - MiniShell++ entry point and read-eval loop.
//
// Usage:
//   ./minishell                 interactive shell (job control enabled on a tty)
//   ./minishell script.msh      run commands from a file
//   ./minishell -c "command"    run a single command line

#include "shell.h"
#include "signals.h"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::strcmp(argv[1], "-c") == 0) {
        if (argc < 3) {
            std::cerr << "minishell: -c: option requires an argument\n";
            return 2;
        }
        initShell(false);
        runLine(argv[2]);
        return g_shell.exitCode >= 0 ? g_shell.exitCode : g_shell.lastStatus;
    }

    int inputFd = STDIN_FILENO;
    if (argc >= 2) {
        inputFd = open(argv[1], O_RDONLY | O_CLOEXEC);
        if (inputFd < 0) {
            std::cerr << "minishell: " << argv[1] << ": " << std::strerror(errno) << '\n';
            return 127;
        }
    }

    bool interactive = inputFd == STDIN_FILENO && isatty(STDIN_FILENO);
    initShell(interactive);

    if (interactive)
        std::cout << "MiniShell++ v0.8 - a lightweight Unix shell in C++. Type 'help' for built-ins.\n";

    while (g_shell.running) {
        if (g_childChanged) {
            g_childChanged = 0;
            g_shell.jobs.reap();
        }
        g_shell.jobs.notify(interactive);

        if (interactive) std::cout << buildPrompt() << std::flush;

        std::string line;
        int r = readLine(inputFd, line);
        if (r == 0) {   // EOF (Ctrl-D)
            if (interactive && g_shell.jobs.hasStopped() && !g_shell.warnedStoppedJobs) {
                std::cout << "\nThere are stopped jobs.\n";
                g_shell.warnedStoppedJobs = true;
                continue;
            }
            if (interactive) std::cout << "exit\n";
            break;
        }
        if (r < 0) {    // Ctrl-C at the prompt
            std::cout << '\n';
            continue;
        }
        runLine(line);
    }

    shutdownShell();
    return g_shell.exitCode >= 0 ? g_shell.exitCode : g_shell.lastStatus;
}
