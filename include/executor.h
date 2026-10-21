// executor.h - process creation, pipes, redirection and foreground waiting.
#pragma once

#include "jobs.h"
#include "parser.h"

// Runs a parsed pipeline and returns its exit status.
int executePipeline(const Pipeline& pipeline);

// Gives the terminal to `job`, optionally sends SIGCONT, and waits until it
// finishes or stops. Returns the job's exit status.
int waitForeground(Job& job, bool resume);
