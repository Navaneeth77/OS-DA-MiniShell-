// executor.cpp - runs pipelines using fork / pipe / dup2 / execvp / waitpid.
//
// For `a | b | c` the shell creates one pipe between each pair of commands and
// forks one child per command. All children join the same process group (the
// "job"), which is handed the terminal with tcsetpgrp() when it runs in the
// foreground so that Ctrl-C / Ctrl-Z reach the job and not the shell.

#include "executor.h"
#include "builtins.h"
#include "shell.h"
#include "signals.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void flushAll() {
    std::cout.flush();
    std::cerr.flush();
    std::fflush(nullptr);
}

// Applies redirections in the order they were written. Returns false (after
// printing an error) if a file cannot be opened.
bool applyRedirections(const std::vector<Redirect>& redirects) {
    for (const Redirect& r : redirects) {
        if (r.kind == Redirect::Kind::Duplicate) {
            if (dup2(r.sourceFd, r.fd) < 0) {
                std::cerr << "minishell: " << r.sourceFd << ": " << std::strerror(errno) << '\n';
                return false;
            }
            continue;
        }
        int flags = O_RDONLY;
        if (r.kind == Redirect::Kind::Write) flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (r.kind == Redirect::Kind::Append) flags = O_WRONLY | O_CREAT | O_APPEND;

        int fd = open(r.target.c_str(), flags, 0644);
        if (fd < 0) {
            std::cerr << "minishell: " << r.target << ": " << std::strerror(errno) << '\n';
            return false;
        }
        if (fd != r.fd) {
            dup2(fd, r.fd);
            close(fd);
        }
    }
    return true;
}

// Builtins such as cd/export/exit must change the shell itself, so a lone
// foreground builtin runs in the shell process. Redirected descriptors are
// saved first and restored afterwards.
int runBuiltinInShell(const Command& cmd) {
    flushAll();
    std::map<int, int> saved;   // fd -> backup copy (-1 if fd was closed)
    for (const Redirect& r : cmd.redirects)
        if (!saved.count(r.fd)) saved[r.fd] = fcntl(r.fd, F_DUPFD_CLOEXEC, 10);

    int status = applyRedirections(cmd.redirects) ? runBuiltin(cmd.argv) : 1;

    flushAll();
    for (auto& [fd, backup] : saved) {
        if (backup >= 0) {
            dup2(backup, fd);
            close(backup);
        } else {
            close(fd);
        }
    }
    return status;
}

// Runs inside the forked child. Never returns.
[[noreturn]] void runChild(const Command& cmd) {
    if (!applyRedirections(cmd.redirects)) _exit(1);

    if (isBuiltin(cmd.argv[0])) {
        int status = runBuiltin(cmd.argv);
        flushAll();
        _exit(status);
    }

    std::vector<char*> args;
    for (const std::string& a : cmd.argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    execvp(args[0], args.data());

    int err = errno;
    if (err == ENOENT && cmd.argv[0].find('/') == std::string::npos) {
        std::cerr << "minishell: " << cmd.argv[0] << ": command not found\n";
        _exit(127);
    }
    std::cerr << "minishell: " << cmd.argv[0] << ": " << std::strerror(err) << '\n';
    _exit(err == ENOENT ? 127 : 126);
}

}  // namespace

int executePipeline(const Pipeline& pl) {
    if (pl.commands.empty()) return g_shell.lastStatus;

    if (pl.commands.size() == 1 && !pl.background && isBuiltin(pl.commands[0].argv[0]))
        return runBuiltinInShell(pl.commands[0]);

    size_t n = pl.commands.size();
    pid_t pgid = 0;
    std::vector<pid_t> pids;
    int inFd = STDIN_FILENO;   // read end feeding the next command

    for (size_t i = 0; i < n; ++i) {
        int pipefd[2] = {-1, -1};
        if (i + 1 < n && pipe(pipefd) < 0) {
            std::perror("minishell: pipe");
            break;
        }

        flushAll();
        pid_t pid = fork();
        if (pid < 0) {
            std::perror("minishell: fork");
            if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
            break;
        }

        if (pid == 0) {
            // ---- child ----
            if (g_shell.interactive) {
                setpgid(0, pgid == 0 ? 0 : pgid);
                if (!pl.background) tcsetpgrp(g_shell.terminalFd, getpgrp());
            }
            resetChildSignals();

            if (inFd != STDIN_FILENO) {
                dup2(inFd, STDIN_FILENO);
                close(inFd);
            }
            if (pipefd[1] >= 0) {
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
                close(pipefd[0]);
            }
            runChild(pl.commands[i]);
        }

        // ---- parent ----
        if (g_shell.interactive) {
            if (pgid == 0) pgid = pid;
            setpgid(pid, pgid);   // also done in the child to avoid a race
        }
        pids.push_back(pid);

        if (inFd != STDIN_FILENO) close(inFd);
        inFd = STDIN_FILENO;
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
            inFd = pipefd[0];
        }
    }
    if (inFd != STDIN_FILENO) close(inFd);

    if (pids.empty()) return 1;
    if (pgid == 0) pgid = pids.front();

    Job& job = g_shell.jobs.add(pgid, pids, pl.text, pl.background, g_shell.tmodes);
    if (pl.background) {
        if (g_shell.interactive) std::cout << '[' << job.id << "] " << pids.back() << std::endl;
        return 0;
    }
    return waitForeground(job, false);
}

int waitForeground(Job& job, bool resume) {
    int fd = g_shell.terminalFd;
    if (g_shell.interactive) {
        tcsetpgrp(fd, job.pgid);
        if (resume) tcsetattr(fd, TCSADRAIN, &job.tmodes);
    }
    if (resume) {
        job.markRunning();
        if (kill(-job.pgid, SIGCONT) < 0) std::perror("minishell: kill (SIGCONT)");
    }
    job.background = false;

    while (job.state() == JobState::Running) {
        int status;
        pid_t pid = waitpid(-1, &status, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR) continue;
            for (Process& p : job.procs) p.completed = true;   // ECHILD: nothing left to wait for
            break;
        }
        g_shell.jobs.updateStatus(pid, status);
    }

    // Take the terminal back and restore the shell's terminal settings.
    if (g_shell.interactive) {
        tcsetpgrp(fd, g_shell.pgid);
        tcgetattr(fd, &job.tmodes);
        tcsetattr(fd, TCSADRAIN, &g_shell.tmodes);
    }

    int status = job.exitStatus();
    if (job.state() == JobState::Stopped) {
        job.notified = true;
        std::cout << "\n[" << job.id << "]+  Stopped                 " << job.command << std::endl;
        return status;
    }

    int raw = job.procs.back().status;
    if (WIFSIGNALED(raw)) {
        int sig = WTERMSIG(raw);
        if (sig == SIGINT) {
            std::cout << std::endl;
        } else if (sig != SIGPIPE) {
            std::cerr << strsignal(sig) << (WCOREDUMP(raw) ? " (core dumped)" : "") << std::endl;
        }
    }
    g_shell.jobs.remove(job.id);
    return status;
}
