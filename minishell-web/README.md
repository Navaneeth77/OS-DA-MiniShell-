# MiniShell++ (C) and its web terminal

MiniShell++ is a Unix shell written in C. `minishell-web` shows it in the browser as an
Ubuntu-style terminal window. It isn't a simulation: the server runs the real shell inside
a **pseudo-terminal**, so echo, Ctrl-C, Ctrl-Z, Ctrl-D, `scanf` input and job control all
come from the kernel, exactly as in a Linux terminal.

## Run

```bash
cd minishell-web
make run
```

Open **http://127.0.0.1:8080** and click the terminal. Stop the server with Ctrl-C.

- `./minishell` runs the same shell directly in any normal terminal.
- `make test` runs 22 end-to-end tests in a real pseudo-terminal.

## Supported commands

| Command | What it does |
|---|---|
| `cd dir`, `cd`, `cd ..`, `cd -` | change directory (built-in) |
| `pwd` | print the working directory (built-in) |
| `mkdir dir` | create a directory |
| `cat file`, `cat >> a.txt` | show a file / append typed lines (finish with **Ctrl-D**) |
| `mv old new` | move or rename |
| `ls`, `ls -l`, `ls -a` | list files |
| `ps`, `ps -aux` (or `ps-aux`) | list processes |
| `pid`, `pid NAME`, `pid %1` | shell PID/PPID/PGID/SID, PIDs by name, PIDs of a job (built-in) |
| `man ls`, `man pid` | manual pages (system pages, plus built-in pages for the shell's own commands) |
| `gedit h.c` | opens a gedit window; the terminal waits until you close it |
| `gcc h.c`, `gcc h.c -o h` | compile C |
| `./a.out` | run the compiled program (interactive `scanf` works) |
| **Ctrl-C** | interrupt the foreground program (SIGINT) |
| **Ctrl-Z** | stop the foreground program (SIGTSTP) |
| `jobs`, `jobs -l` | list stopped/background jobs |
| `fg`, `fg %n` / `bg`, `bg %n` | continue a job in the foreground / background |
| `kill %n`, `kill -9 PID`, `kill -l` | send signals |
| `cmd &` | start in the background |

Also: `clear`, `help`, `exit`, redirection `> >> <`, pipes `|`, ↑/↓ history, ←/→ editing.
Any other command prints `command not supported`.

## Presentation demo

```text
mkdir osda
cd osda
pwd
cat >> a.txt          (type a few lines, then Ctrl-D)
cat a.txt
mv a.txt notes.txt
ls -l
gedit h.c             (write the program, Ctrl+S, close the window)
gcc h.c
ls
./a.out
ps
ps -aux
pid
man ls
gedit h.c             then Ctrl-Z in the terminal: the window stops responding
jobs
fg                    the window works again; close it
```

A good program for showing Ctrl-Z → `jobs` → `fg`/`bg`:

```c
#include <stdio.h>
#include <unistd.h>

int main() {
    for (int i = 1; ; i++) {
        printf("tick %d (pid %d)\n", i, getpid());
        sleep(1);
    }
}
```

Run `./a.out`, press Ctrl-Z, run `jobs`, `pid %1`, `bg` (it keeps printing), `fg`, then Ctrl-C.

> **macOS note:** if you stop a program with Ctrl-Z *while it is waiting for input*
> (`cat`, `scanf`), macOS makes that read fail when it resumes (`Interrupted system call`).
> bash and zsh on macOS do the same. Linux keeps waiting for input. The loop program above
> works the same on both.

## How it works

```
 Browser                              minishell-web (C)                   minishell (C)
 ───────                              ─────────────────                   ─────────────
 key press  ── POST /api/input ──▶    write(pty master) ──▶ kernel tty ──▶ shell / program
                                       (line discipline: echo, ^C→SIGINT, ^Z→SIGTSTP, ^D→EOF)
 screen     ◀─ Server-Sent Events ─   read(pty master)  ◀── output ─────  shell / program
```

- **`src/web/server.c`**
  - Creates the pseudo-terminal with `posix_openpt`/`grantpt`/`unlockpt`.
  - Forks the shell into a new session (`setsid`) with the pty slave as its controlling
    terminal (`TIOCSCTTY`).
  - Relays bytes between the pty and the browser in a single `poll()` loop.
  - Window resizes set `TIOCSWINSZ`, which makes the kernel send `SIGWINCH`.
- **`src/shell/`**, the shell:
  - `exec.c` does `fork`/`pipe`/`dup2`/`execvp`. Each command line gets its own process
    group, and `tcsetpgrp` hands the terminal to the foreground job.
  - `jobs.c` tracks job state with `waitpid(WUNTRACED|WCONTINUED)`.
  - `builtins.c` has `cd`, `pwd`, `pid`, `jobs`, `fg`, `bg`, `kill`, `man`, `gedit` and more.
  - `lineedit.c` switches the terminal to raw mode (`tcsetattr`) for arrow keys and history.
- **`public/terminal.js`** is a small VT100/xterm emulator: cursor movement, colours,
  erase, scrollback and window title.
- **gedit**
  - The `gedit` process prints a private escape sequence, and the page opens an editor
    window for it.
  - The server lets the page read and save only files that a live gedit process opened.
  - Closing the window sends that process `SIGUSR1`, so it exits and the prompt returns.
  - While the process is stopped (Ctrl-Z), the window shows "not responding".

## Files

```
src/common/   parser.c (quotes, $VAR, | < > >> &), sbuf.c (strings)
src/shell/    main.c shell.c exec.c jobs.c builtins.c lineedit.c
src/web/      server.c (pty + HTTP + events), http.c
public/       index.html terminal.css terminal.js gedit.js
tests/        test_terminal.py
```
