// shell.cpp - shell initialisation, prompt, input and history expansion.

#include "shell.h"
#include "executor.h"
#include "parser.h"
#include "signals.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <iostream>
#include <unistd.h>

Shell g_shell;

void initShell(bool interactive) {
    g_shell.interactive = interactive;
    g_shell.terminalFd = STDIN_FILENO;

    if (interactive) {
        // Wait until we are in the foreground before taking over the terminal.
        pid_t pgrp;
        while (tcgetpgrp(g_shell.terminalFd) != (pgrp = getpgrp()))
            kill(-pgrp, SIGTTIN);
    }

    installShellSignals(interactive);

    if (interactive) {
        // Put the shell in its own process group and grab the terminal.
        setpgid(0, 0);   // fails harmlessly if we are already a session leader
        g_shell.pgid = getpgrp();
        tcsetpgrp(g_shell.terminalFd, g_shell.pgid);
        tcgetattr(g_shell.terminalFd, &g_shell.tmodes);
    } else {
        g_shell.pgid = getpgrp();
    }
}

void shutdownShell() {
    // Like bash: stopped jobs would be orphaned, so hang them up.
    for (auto& [id, job] : g_shell.jobs.all()) {
        if (job.state() == JobState::Stopped && g_shell.interactive) {
            kill(-job.pgid, SIGHUP);
            kill(-job.pgid, SIGCONT);
        }
    }
}

std::string buildPrompt() {
    const char* user = std::getenv("USER");
    char host[256] = "localhost";
    gethostname(host, sizeof host);
    std::string hostname(host);
    hostname = hostname.substr(0, hostname.find('.'));

    char cwdBuf[PATH_MAX];
    std::string cwd = getcwd(cwdBuf, sizeof cwdBuf) ? cwdBuf : "?";
    if (const char* home = std::getenv("HOME")) {
        std::string h(home);
        if (!h.empty() && cwd.compare(0, h.size(), h) == 0 && (cwd.size() == h.size() || cwd[h.size()] == '/'))
            cwd = "~" + cwd.substr(h.size());
    }

    std::string status = g_shell.lastStatus == 0 ? "" : "\033[31m[" + std::to_string(g_shell.lastStatus) + "]\033[0m ";
    return "\033[1;35mminishell++\033[0m " + status + "\033[1;32m" + (user ? user : "user") + "@" + hostname +
           "\033[0m:\033[1;34m" + cwd + "\033[0m$ ";
}

int readLine(int fd, std::string& line) {
    static std::string pending;   // bytes read past the previous newline
    line.clear();
    for (;;) {
        size_t nl = pending.find('\n');
        if (nl != std::string::npos) {
            line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            return 1;
        }
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                if (g_sigintReceived) {
                    g_sigintReceived = 0;
                    pending.clear();
                    return -1;
                }
                continue;   // e.g. SIGCHLD
            }
            return 0;
        }
        if (n == 0) {
            if (pending.empty()) return 0;
            line.swap(pending);
            pending.clear();
            return 1;
        }
        pending.append(buf, static_cast<size_t>(n));
    }
}

namespace {

// Expands !! (last command), !N (Nth command) and !-N (N commands back).
bool expandHistory(const std::string& line, std::string& out) {
    const auto& history = g_shell.history;
    out.clear();
    bool changed = false, inSingle = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '\'') inSingle = !inSingle;
        if (c == '\\' && i + 1 < line.size()) {
            out += c;
            out += line[++i];
            continue;
        }
        if (c == '!' && !inSingle && i + 1 < line.size()) {
            if (line[i + 1] == '!') {
                if (history.empty()) {
                    std::cerr << "minishell: !!: event not found\n";
                    return false;
                }
                out += history.back();
                ++i;
                changed = true;
                continue;
            }
            size_t j = i + 1;
            bool negative = line[j] == '-';
            if (negative) ++j;
            size_t digitsStart = j;
            while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])) && j - digitsStart < 9) ++j;
            if (j > digitsStart) {
                long num = std::stol(line.substr(digitsStart, j - digitsStart));
                long idx = negative ? static_cast<long>(history.size()) - num : num - 1;
                if (idx < 0 || idx >= static_cast<long>(history.size())) {
                    std::cerr << "minishell: " << line.substr(i, j - i) << ": event not found\n";
                    return false;
                }
                out += history[static_cast<size_t>(idx)];
                i = j - 1;
                changed = true;
                continue;
            }
        }
        out += c;
    }
    if (changed) std::cout << out << std::endl;
    return true;
}

}  // namespace

void runLine(const std::string& rawLine) {
    std::string line;
    if (!expandHistory(rawLine, line)) {
        g_shell.lastStatus = 1;
        return;
    }
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) return;
    g_shell.history.push_back(line);

    Pipeline pipeline;
    std::string error;
    if (!parseLine(line, pipeline, error)) {
        std::cerr << "minishell: " << error << '\n';
        g_shell.lastStatus = 2;
        return;
    }
    if (pipeline.commands.empty()) return;

    if (pipeline.commands[0].argv[0] != "exit") g_shell.warnedStoppedJobs = false;
    g_shell.lastStatus = executePipeline(pipeline);
}
