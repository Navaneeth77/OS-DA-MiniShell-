// builtins.cpp - commands executed by the shell itself.

#include "builtins.h"
#include "executor.h"
#include "shell.h"
#include "signals.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

using Args = std::vector<std::string>;
using BuiltinFn = int (*)(const Args&);

int fail(const std::string& name, const std::string& msg) {
    std::cerr << "minishell: " << name << ": " << msg << '\n';
    return 1;
}

bool parseLong(const std::string& s, long& value) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    value = std::strtol(s.c_str(), &end, 10);
    return errno == 0 && *end == '\0';
}

bool isValidName(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::all_of(name.begin(), name.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

std::string findInPath(const std::string& name) {
    auto executable = [](const std::string& path) {
        struct stat st {};
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
    };
    if (name.find('/') != std::string::npos) return executable(name) ? name : "";
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string candidate = (dir.empty() ? "." : dir) + "/" + name;
        if (executable(candidate)) return candidate;
    }
    return "";
}

// ------------------------------------------------------------ signals

struct SignalName { const char* name; int number; };
const SignalName kSignals[] = {
    {"HUP", SIGHUP},   {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"KILL", SIGKILL},
    {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, {"ALRM", SIGALRM},
    {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, {"STOP", SIGSTOP},
    {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
};

int signalFromSpec(std::string spec) {
    long num;
    if (parseLong(spec, num)) return (num > 0 && num < NSIG) ? static_cast<int>(num) : -1;
    std::transform(spec.begin(), spec.end(), spec.begin(), [](unsigned char c) { return std::toupper(c); });
    if (spec.rfind("SIG", 0) == 0) spec = spec.substr(3);
    for (const SignalName& s : kSignals)
        if (spec == s.name) return s.number;
    return -1;
}

// ----------------------------------------------------------- job specs

// Resolves "%n", "n", "%%", "%+", "%-" or no argument to a job.
Job* resolveJob(const Args& args, const std::string& builtin) {
    JobTable& jobs = g_shell.jobs;
    if (args.size() < 2 || args[1] == "%%" || args[1] == "%+") {
        Job* job = jobs.current();
        if (!job) fail(builtin, "current: no such job");
        return job;
    }
    if (args[1] == "%-") {
        Job* job = jobs.previous();
        if (!job) fail(builtin, "previous: no such job");
        return job;
    }
    std::string spec = args[1][0] == '%' ? args[1].substr(1) : args[1];
    long id;
    Job* job = parseLong(spec, id) ? jobs.find(static_cast<int>(id)) : nullptr;
    if (!job) fail(builtin, args[1] + ": no such job");
    return job;
}

// ------------------------------------------------------------ builtins

int builtinCd(const Args& a) {
    if (a.size() > 2) return fail("cd", "too many arguments");
    std::string target;
    bool printDir = false;
    if (a.size() == 1) {
        const char* home = std::getenv("HOME");
        if (!home) return fail("cd", "HOME not set");
        target = home;
    } else if (a[1] == "-") {
        const char* old = std::getenv("OLDPWD");
        if (!old) return fail("cd", "OLDPWD not set");
        target = old;
        printDir = true;
    } else {
        target = a[1];
    }

    char oldCwd[PATH_MAX];
    bool haveOld = getcwd(oldCwd, sizeof oldCwd) != nullptr;
    if (chdir(target.c_str()) < 0) return fail("cd", target + ": " + std::strerror(errno));

    if (haveOld) setenv("OLDPWD", oldCwd, 1);
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) {
        setenv("PWD", cwd, 1);
        if (printDir) std::cout << cwd << '\n';
    }
    return 0;
}

int builtinPwd(const Args&) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return fail("pwd", std::strerror(errno));
    std::cout << cwd << '\n';
    return 0;
}

int builtinEcho(const Args& a) {
    size_t i = 1;
    bool newline = true;
    if (a.size() > 1 && a[1] == "-n") { newline = false; i = 2; }
    for (; i < a.size(); ++i) {
        std::cout << a[i];
        if (i + 1 < a.size()) std::cout << ' ';
    }
    if (newline) std::cout << '\n';
    return 0;
}

int builtinExit(const Args& a) {
    if (g_shell.interactive && g_shell.jobs.hasStopped() && !g_shell.warnedStoppedJobs) {
        std::cerr << "There are stopped jobs.\n";
        g_shell.warnedStoppedJobs = true;
        return 1;
    }
    int code = g_shell.lastStatus;
    if (a.size() > 1) {
        long v;
        if (!parseLong(a[1], v)) {
            fail("exit", a[1] + ": numeric argument required");
            code = 2;
        } else if (a.size() > 2) {
            return fail("exit", "too many arguments");
        } else {
            code = static_cast<int>(v & 0xFF);
        }
    }
    g_shell.exitCode = code;
    g_shell.running = false;
    return code;
}

int builtinExport(const Args& a) {
    if (a.size() == 1) {
        std::vector<std::string> vars;
        for (char** e = environ; *e; ++e) vars.emplace_back(*e);
        std::sort(vars.begin(), vars.end());
        for (const std::string& v : vars) {
            size_t eq = v.find('=');
            std::cout << "declare -x " << v.substr(0, eq) << "=\"" << v.substr(eq + 1) << "\"\n";
        }
        return 0;
    }
    int status = 0;
    for (size_t i = 1; i < a.size(); ++i) {
        size_t eq = a[i].find('=');
        std::string name = a[i].substr(0, eq);
        if (!isValidName(name)) {
            status = fail("export", "`" + a[i] + "': not a valid identifier");
            continue;
        }
        if (eq != std::string::npos) setenv(name.c_str(), a[i].c_str() + eq + 1, 1);
    }
    return status;
}

int builtinUnset(const Args& a) {
    int status = 0;
    for (size_t i = 1; i < a.size(); ++i) {
        if (!isValidName(a[i])) {
            status = fail("unset", "`" + a[i] + "': not a valid identifier");
            continue;
        }
        unsetenv(a[i].c_str());
    }
    return status;
}

int builtinHistory(const Args& a) {
    auto& history = g_shell.history;
    if (a.size() > 1 && a[1] == "-c") {
        history.clear();
        return 0;
    }
    size_t start = 0;
    if (a.size() > 1) {
        long count;
        if (!parseLong(a[1], count) || count < 0) return fail("history", a[1] + ": numeric argument required");
        if (static_cast<size_t>(count) < history.size()) start = history.size() - count;
    }
    for (size_t i = start; i < history.size(); ++i)
        std::cout << std::setw(5) << i + 1 << "  " << history[i] << '\n';
    return 0;
}

int builtinJobs(const Args& a) {
    g_shell.jobs.reap();
    g_shell.jobs.list(a.size() > 1 && a[1] == "-l");
    return 0;
}

int builtinFg(const Args& a) {
    if (!g_shell.interactive) return fail("fg", "no job control");
    g_shell.jobs.reap();
    Job* job = resolveJob(a, "fg");
    if (!job) return 1;
    if (job->state() == JobState::Done) return fail("fg", "job has terminated");
    std::cout << job->command << std::endl;
    return waitForeground(*job, true);
}

int builtinBg(const Args& a) {
    if (!g_shell.interactive) return fail("bg", "no job control");
    g_shell.jobs.reap();
    Job* job = resolveJob(a, "bg");
    if (!job) return 1;
    if (job->state() == JobState::Done) return fail("bg", "job has terminated");
    if (job->state() == JobState::Running) {
        std::cerr << "minishell: bg: job " << job->id << " already in background\n";
        return 0;
    }
    job->markRunning();
    job->background = true;
    if (kill(-job->pgid, SIGCONT) < 0) return fail("bg", std::strerror(errno));
    std::cout << '[' << job->id << "]+ " << job->command << " &\n";
    return 0;
}

int builtinKill(const Args& a) {
    const char* usage = "usage: kill [-s sigspec | -signum | -sigspec] pid | %job ... or kill -l";
    if (a.size() < 2) return fail("kill", usage);

    if (a[1] == "-l") {
        for (const SignalName& s : kSignals) std::cout << std::setw(2) << s.number << ") SIG" << s.name << '\n';
        return 0;
    }

    int sig = SIGTERM;
    size_t i = 1;
    if (a[1] == "-s") {
        if (a.size() < 3) return fail("kill", usage);
        sig = signalFromSpec(a[2]);
        i = 3;
    } else if (a[1].size() > 1 && a[1][0] == '-') {
        sig = signalFromSpec(a[1].substr(1));
        i = 2;
    }
    if (sig < 0) return fail("kill", "invalid signal specification");
    if (i >= a.size()) return fail("kill", usage);

    int status = 0;
    for (; i < a.size(); ++i) {
        const std::string& target = a[i];
        if (target[0] == '%') {
            long id;
            Job* job = parseLong(target.substr(1), id) ? g_shell.jobs.find(static_cast<int>(id)) : nullptr;
            if (!job) { status = fail("kill", target + ": no such job"); continue; }
            bool ok = true;
            if (g_shell.interactive) {
                ok = kill(-job->pgid, sig) == 0;
            } else {
                for (const Process& p : job->procs)
                    if (!p.completed && kill(p.pid, sig) < 0) ok = false;
            }
            if (!ok) { status = fail("kill", target + ": " + std::strerror(errno)); continue; }
            // A stopped job can't act on a terminating signal until it is continued.
            if (job->state() == JobState::Stopped && sig != SIGSTOP && sig != SIGTSTP && sig != SIGCONT)
                kill(g_shell.interactive ? -job->pgid : job->pgid, SIGCONT);
        } else {
            long pid;
            if (!parseLong(target, pid)) {
                status = fail("kill", target + ": arguments must be process or job IDs");
                continue;
            }
            if (kill(static_cast<pid_t>(pid), sig) < 0) status = fail("kill", "(" + target + ") - " + std::strerror(errno));
        }
    }
    return status;
}

int builtinWait(const Args&) {
    while (g_shell.jobs.hasRunning()) {
        int st;
        pid_t pid = waitpid(-1, &st, WUNTRACED);
        if (pid < 0) {
            if (errno == EINTR && !g_sigintReceived) continue;
            break;
        }
        g_shell.jobs.updateStatus(pid, st);
    }
    g_sigintReceived = 0;
    g_shell.jobs.notify(g_shell.interactive);
    return 0;
}

int builtinType(const Args& a) {
    int status = 0;
    for (size_t i = 1; i < a.size(); ++i) {
        if (isBuiltin(a[i])) {
            std::cout << a[i] << " is a shell builtin\n";
        } else if (std::string path = findInPath(a[i]); !path.empty()) {
            std::cout << a[i] << " is " << path << '\n';
        } else {
            status = fail("type", a[i] + ": not found");
        }
    }
    return status;
}

int builtinHelp(const Args&) {
    std::cout <<
        "MiniShell++ built-in commands\n"
        "  cd [dir | -]            change the working directory\n"
        "  pwd                     print the working directory\n"
        "  echo [-n] args...       print arguments\n"
        "  export [NAME=VALUE]     set / list environment variables\n"
        "  unset NAME...           remove environment variables\n"
        "  history [-c | N]        show or clear command history (!! and !N re-run)\n"
        "  jobs [-l]               list background and stopped jobs\n"
        "  fg [%job]               bring a job to the foreground\n"
        "  bg [%job]               resume a stopped job in the background\n"
        "  kill [-SIG] pid|%job    send a signal (kill -l lists signals)\n"
        "  wait                    wait for all background jobs\n"
        "  type name...            show how a name would be interpreted\n"
        "  help                    show this message\n"
        "  exit [n]                leave the shell\n"
        "\n"
        "Operators:  cmd1 | cmd2   < file   > file   >> file   2> file   2>&1   cmd &\n"
        "Expansion:  'literal'  \"$VAR\"  ${VAR}  $?  $$  ~  \\escape  # comment\n"
        "Keys:       Ctrl-C interrupts, Ctrl-Z stops the foreground job, Ctrl-D exits\n";
    return 0;
}

const std::map<std::string, BuiltinFn>& table() {
    static const std::map<std::string, BuiltinFn> builtins = {
        {"cd", builtinCd},       {"pwd", builtinPwd},     {"echo", builtinEcho},
        {"exit", builtinExit},   {"export", builtinExport}, {"unset", builtinUnset},
        {"history", builtinHistory}, {"jobs", builtinJobs}, {"fg", builtinFg},
        {"bg", builtinBg},       {"kill", builtinKill},   {"wait", builtinWait},
        {"type", builtinType},   {"help", builtinHelp},
    };
    return builtins;
}

}  // namespace

bool isBuiltin(const std::string& name) { return table().count(name) > 0; }

int runBuiltin(const std::vector<std::string>& argv) {
    auto it = table().find(argv[0]);
    return it == table().end() ? 127 : it->second(argv);
}
