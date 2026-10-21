// jobs.cpp - bookkeeping for running, stopped and finished jobs.

#include "jobs.h"
#include "shell.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/wait.h>

int statusToExitCode(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFSTOPPED(status)) return 128 + WSTOPSIG(status);
    return 0;
}

// ---------------------------------------------------------------- Job

JobState Job::state() const {
    bool allDone = true, anyRunning = false;
    for (const Process& p : procs) {
        if (p.completed) continue;
        allDone = false;
        if (!p.stopped) anyRunning = true;
    }
    if (allDone) return JobState::Done;
    return anyRunning ? JobState::Running : JobState::Stopped;
}

int Job::exitStatus() const {
    return procs.empty() ? 0 : statusToExitCode(procs.back().status);
}

std::string Job::describeState() const {
    switch (state()) {
    case JobState::Running: return "Running";
    case JobState::Stopped: return "Stopped";
    case JobState::Done: break;
    }
    int st = procs.back().status;
    if (WIFSIGNALED(st)) return strsignal(WTERMSIG(st));
    if (WIFEXITED(st) && WEXITSTATUS(st) != 0) return "Exit " + std::to_string(WEXITSTATUS(st));
    return "Done";
}

void Job::markRunning() {
    for (Process& p : procs) p.stopped = false;
    notified = false;
}

// ----------------------------------------------------------- JobTable

Job& JobTable::add(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command,
                   bool background, const struct termios& tmodes) {
    int id = jobs_.empty() ? 1 : jobs_.rbegin()->first + 1;
    Job job;
    job.id = id;
    job.pgid = pgid;
    job.command = command;
    job.background = background;
    job.tmodes = tmodes;
    for (pid_t pid : pids) {
        Process p;
        p.pid = pid;
        job.procs.push_back(p);
    }
    return jobs_[id] = job;
}

Job* JobTable::find(int id) {
    auto it = jobs_.find(id);
    return it == jobs_.end() ? nullptr : &it->second;
}

Job* JobTable::current() {
    return jobs_.empty() ? nullptr : &jobs_.rbegin()->second;
}

Job* JobTable::previous() {
    if (jobs_.size() < 2) return nullptr;
    return &std::next(jobs_.rbegin())->second;
}

void JobTable::remove(int id) { jobs_.erase(id); }

void JobTable::updateStatus(pid_t pid, int status) {
    for (auto& [id, job] : jobs_) {
        for (Process& p : job.procs) {
            if (p.pid != pid) continue;
            if (WIFSTOPPED(status)) {
                p.stopped = true;
                p.status = status;
            } else if (WIFCONTINUED(status)) {
                p.stopped = false;
            } else {
                p.completed = true;
                p.stopped = false;
                p.status = status;
            }
            job.notified = false;
            return;
        }
    }
}

void JobTable::reap() {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0)
        updateStatus(pid, status);
}

std::string JobTable::formatLine(const Job& job, bool showPids) {
    char mark = ' ';
    if (current() == &job) mark = '+';
    else if (previous() == &job) mark = '-';

    std::ostringstream os;
    os << '[' << job.id << ']' << mark << "  ";
    if (showPids) os << job.pgid << ' ';
    std::string state = job.describeState();
    os << state << std::string(state.size() < 24 ? 24 - state.size() : 1, ' ') << job.command;
    if (job.state() == JobState::Running && job.background) os << " &";
    return os.str();
}

void JobTable::notify(bool announce) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        Job& job = it->second;
        JobState st = job.state();
        if (st == JobState::Done) {
            if (announce && !job.notified) std::cout << formatLine(job, false) << '\n';
            it = jobs_.erase(it);
            continue;
        }
        if (st == JobState::Stopped && !job.notified) {
            if (announce) std::cout << formatLine(job, false) << '\n';
            job.notified = true;
        }
        ++it;
    }
    std::cout.flush();
}

void JobTable::list(bool showPids) {
    for (auto& [id, job] : jobs_) {
        std::cout << formatLine(job, showPids) << '\n';
        if (job.state() != JobState::Running) job.notified = true;
    }
    notify(false);   // finished jobs were just shown, so drop them silently
}

bool JobTable::hasStopped() const {
    for (const auto& [id, job] : jobs_)
        if (job.state() == JobState::Stopped) return true;
    return false;
}

bool JobTable::hasRunning() const {
    for (const auto& [id, job] : jobs_)
        if (job.state() == JobState::Running) return true;
    return false;
}
