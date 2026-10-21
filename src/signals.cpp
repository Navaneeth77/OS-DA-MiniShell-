// signals.cpp - signal handling.
//
// The interactive shell must survive Ctrl-C / Ctrl-Z typed at the prompt and
// must not be stopped when it touches the terminal from the background, so it
// ignores the job-control signals. Children get default dispositions back so
// that Ctrl-C / Ctrl-Z act on them normally.

#include "signals.h"

#include <initializer_list>
#include <signal.h>

volatile sig_atomic_t g_sigintReceived = 0;
volatile sig_atomic_t g_childChanged = 0;

namespace {

void onSigint(int) { g_sigintReceived = 1; }
void onSigchld(int) { g_childChanged = 1; }

void setHandler(int sig, void (*handler)(int), int flags) {
    struct sigaction sa {};
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

}  // namespace

void installShellSignals(bool interactive) {
    // SIGCHLD only raises a flag; the actual waitpid() happens in the main loop
    // where it is safe to touch the job table. SA_RESTART keeps read() going.
    setHandler(SIGCHLD, onSigchld, SA_RESTART);
    if (!interactive) return;

    // No SA_RESTART: Ctrl-C interrupts read() so the prompt can be redrawn.
    setHandler(SIGINT, onSigint, 0);
    setHandler(SIGQUIT, SIG_IGN, 0);
    setHandler(SIGTSTP, SIG_IGN, 0);
    setHandler(SIGTTIN, SIG_IGN, 0);
    setHandler(SIGTTOU, SIG_IGN, 0);
}

void resetChildSignals() {
    for (int sig : {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGCHLD, SIGPIPE})
        setHandler(sig, SIG_DFL, 0);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
}
