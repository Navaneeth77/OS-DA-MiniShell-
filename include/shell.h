// shell.h - global shell state shared by all modules.
#pragma once

#include <csignal>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>

#include "jobs.h"

struct Shell {
    bool interactive = false;       // attached to a terminal -> job control enabled
    int terminalFd = STDIN_FILENO;
    pid_t pgid = 0;                 // shell's own process group
    struct termios tmodes {};       // shell's terminal modes
    int lastStatus = 0;             // $?
    int exitCode = -1;              // set by `exit`
    bool running = true;
    bool warnedStoppedJobs = false;
    JobTable jobs;
    std::vector<std::string> history;
};

extern Shell g_shell;

void initShell(bool interactive);
void runLine(const std::string& line);
std::string buildPrompt();
// Reads one line from fd. Returns 1 on success, 0 on EOF, -1 if interrupted by Ctrl-C.
int readLine(int fd, std::string& line);
void shutdownShell();
