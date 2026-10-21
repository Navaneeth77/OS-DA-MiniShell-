// jobs.h - job table used for background processes and job control.
#pragma once

#include <map>
#include <string>
#include <vector>
#include <sys/types.h>
#include <termios.h>

enum class JobState { Running, Stopped, Done };

struct Process {
    pid_t pid = 0;
    bool completed = false;
    bool stopped = false;
    int status = 0;        // raw status from waitpid()
};

struct Job {
    int id = 0;
    pid_t pgid = 0;                 // process group of the pipeline
    std::string command;
    std::vector<Process> procs;
    bool background = false;
    bool notified = false;          // user already told about the latest state change
    struct termios tmodes {};       // terminal modes saved when the job was stopped

    JobState state() const;
    int exitStatus() const;         // shell-style status ($?) of the last process
    std::string describeState() const;
    void markRunning();
};

class JobTable {
public:
    Job& add(pid_t pgid, const std::vector<pid_t>& pids, const std::string& command,
             bool background, const struct termios& tmodes);
    Job* find(int id);
    Job* current();                 // "%+" : most recent job
    Job* previous();                // "%-" : the one before it
    void remove(int id);

    void updateStatus(pid_t pid, int status);   // record a waitpid() result
    void reap();                                // non-blocking collection of child status changes
    void notify(bool announce);                 // report Done/Stopped jobs, drop finished ones
    void list(bool showPids);                   // output of the `jobs` builtin

    bool hasStopped() const;
    bool hasRunning() const;
    std::map<int, Job>& all() { return jobs_; }

private:
    std::string formatLine(const Job& job, bool showPids);
    std::map<int, Job> jobs_;
};

// Converts a raw waitpid() status into a shell exit code (0-255, 128+signal).
int statusToExitCode(int status);
