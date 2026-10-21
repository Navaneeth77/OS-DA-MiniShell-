# MiniShell++

A lightweight Unix-like command interpreter written in C++17. It emulates the core of Bash
(running commands, pipes, redirection, background jobs, job control) while staying small
enough to read end to end. It is meant as a teaching implementation of OS concepts.

**Status: DA-2 (about 80% of the planned features)**

---

## Build & run

```bash
make            # builds ./minishell
./minishell     # interactive shell
make test       # runs the automated test suite (36 tests)
```

Other modes:

```bash
./minishell -c "ls -l | grep cpp | wc -l"   # run one command line
./minishell script.msh                      # run commands from a file
```

Needs a POSIX system (Linux or macOS) and a C++17 compiler.

---

## Features implemented (DA-2)

| Area | What works |
|---|---|
| **Command execution** | Runs any program on `PATH` or by path with `fork()` + `execvp()`. `command not found` gives exit status 127 and a permission error gives 126. |
| **Parsing** | `'single'` and `"double"` quotes, backslash escapes, `$VAR`, `${VAR}`, `$?`, `$$`, `~` expansion, `# comments`, and syntax-error reporting. |
| **Pipes** | Pipelines of any length, e.g. `cat f \| sort \| uniq -c \| sort -nr \| head`. |
| **I/O redirection** | `<`, `>`, `>>`, `2>`, `2>>`, `N>file`, `2>&1` / `N>&M`, applied left to right. They also work on built-ins. |
| **Background processes** | `cmd &` prints `[job] pid`, and the shell reports `Done` / `Exit N` / signal names when the job ends. |
| **Job control** | Every pipeline gets its own process group. `Ctrl-Z` stops a job, `Ctrl-C` interrupts it, and `jobs [-l]`, `fg [%n]`, `bg [%n]` and `kill [-SIG] pid\|%n` manage jobs. The terminal is handed over with `tcsetpgrp()`. |
| **Signal handling** | The shell ignores `SIGTSTP`/`SIGTTIN`/`SIGTTOU`/`SIGQUIT`. `SIGINT` at the prompt redraws the prompt. `SIGCHLD` flags status changes for reaping. Children get default signal handling back. |
| **Built-ins** | `cd [dir\|-]`, `pwd`, `echo [-n]`, `export`, `unset`, `history [-c\|N]`, `jobs`, `fg`, `bg`, `kill`, `wait`, `type`, `help`, `exit [n]` |
| **History** | In-memory history, plus `!!`, `!N` and `!-N` expansion. |
| **Prompt** | Coloured `user@host:cwd$` prompt that shows the last non-zero exit status. |

## Remaining work (final DA)

- Command lists and conditionals: `;`, `&&`, `||` (the parser currently reports "not supported yet")
- Wildcard / glob expansion (`*.cpp`, `?`, `[a-z]`)
- Line editing: arrow-key history navigation and tab completion
- Persistent history file (`~/.minishell_history`) and a startup file (`~/.minishellrc`)
- Here-documents (`<<EOF`), command substitution `$(...)`, subshells `( ... )`
- Shell variables that are not exported, plus `VAR=value cmd` prefix assignments

---

## Project structure

```
include/            headers
  parser.h          Command / Redirect / Pipeline structures
  jobs.h            Job, Process, JobTable
  executor.h        pipeline execution API
  builtins.h        builtin dispatch
  signals.h         signal setup
  shell.h           global shell state
src/
  main.cpp          argument handling + read-eval loop
  shell.cpp         initialisation, prompt, input, history expansion
  parser.cpp        tokenizer + parser
  executor.cpp      fork/pipe/dup2/exec, redirection, foreground waiting
  jobs.cpp          job table, status tracking, notifications
  builtins.cpp      all built-in commands
  signals.cpp       signal handlers
tests/run_tests.sh  automated tests
```

### How a command line flows through the shell

```
read line ──► history expansion (!!, !N) ──► tokenizer (quotes, $VAR, ~, operators)
         ──► parser (commands, redirects, |, &) ──► executor
                 ├─ single built-in in foreground → run inside the shell (cd, export...)
                 └─ otherwise, for each command:
                        pipe() → fork() → child: setpgid, tcsetpgrp, reset signals,
                                                 dup2 pipe ends, apply redirects, execvp
                                          parent: setpgid, close pipe ends
                    → add to JobTable → background? print [id] pid : waitForeground()
```

### OS concepts → system calls used

| Concept | System calls |
|---|---|
| Process creation | `fork`, `execvp`, `_exit` |
| Process termination / status | `waitpid` (`WUNTRACED`, `WNOHANG`, `WCONTINUED`), `WIFEXITED`, `WIFSIGNALED`, `WIFSTOPPED` |
| Inter-process communication | `pipe` |
| File descriptor management | `open`, `dup2`, `close`, `fcntl(F_DUPFD_CLOEXEC)` |
| Process groups & terminals | `setpgid`, `getpgrp`, `tcsetpgrp`, `tcgetpgrp`, `tcgetattr`, `tcsetattr` |
| Signals | `sigaction`, `sigprocmask`, `kill` (with negative pgid for whole jobs) |
| Filesystem / environment | `chdir`, `getcwd`, `setenv`, `unsetenv`, `access`, `stat` |

---

## Sample session

```
minishell++ user@host:~/OS-Da$ ls src | grep .cpp | wc -l
       7
minishell++ user@host:~/OS-Da$ echo "home is $HOME" > out.txt; cat < out.txt
minishell: `;' is not supported yet
minishell++ [2] user@host:~/OS-Da$ sort -r < out.txt >> sorted.txt 2>&1
minishell++ user@host:~/OS-Da$ sleep 30
^Z
[1]+  Stopped                 sleep 30
minishell++ [146] user@host:~/OS-Da$ bg
[1]+ sleep 30 &
minishell++ user@host:~/OS-Da$ jobs -l
[1]+  46415 Running                 sleep 30 &
minishell++ user@host:~/OS-Da$ fg %1
sleep 30
^C
minishell++ [130] user@host:~/OS-Da$ nosuchcmd
minishell: nosuchcmd: command not found
minishell++ [127] user@host:~/OS-Da$ exit
```
