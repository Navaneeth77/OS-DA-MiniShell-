// signals.h - signal setup for the shell and for spawned children.
#pragma once

#include <csignal>

extern volatile sig_atomic_t g_sigintReceived;
extern volatile sig_atomic_t g_childChanged;

// Shell: catch SIGINT/SIGCHLD, ignore SIGQUIT/SIGTSTP/SIGTTIN/SIGTTOU when interactive.
void installShellSignals(bool interactive);
// Child (after fork, before exec): restore default dispositions and unblock everything.
void resetChildSignals();
