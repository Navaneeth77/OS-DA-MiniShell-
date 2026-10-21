// parser.h - tokenizer and parser for MiniShell++ command lines.
#pragma once

#include <string>
#include <vector>

// A single I/O redirection attached to a command, applied in order.
struct Redirect {
    enum class Kind { Read, Write, Append, Duplicate };
    Kind kind = Kind::Write;
    int fd = 1;            // descriptor being redirected (0 = stdin, 1 = stdout, 2 = stderr)
    std::string target;    // file name for Read / Write / Append
    int sourceFd = -1;     // for Duplicate: descriptor to copy (2>&1 -> fd 2, sourceFd 1)
};

// One simple command inside a pipeline: argv plus its redirections.
struct Command {
    std::vector<std::string> argv;
    std::vector<Redirect> redirects;
};

// A full command line: cmd1 | cmd2 | ... [&]
struct Pipeline {
    std::vector<Command> commands;
    bool background = false;
    std::string text;      // original text, used for job listings
};

// Parses `line` into `out`. Returns false and fills `error` on a syntax error.
// An empty/comment-only line yields a pipeline with no commands.
bool parseLine(const std::string& line, Pipeline& out, std::string& error);
