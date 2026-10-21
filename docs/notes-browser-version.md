# MiniShell++ — a Linux terminal in one HTML file

**The whole project is a single file: [`minishell.html`](minishell.html) (about 5,600 lines).**

Double-click it. It opens in any browser on any computer — Windows, macOS, Linux, a phone,
a college lab machine — with nothing installed, no server, and no internet connection.
Upload it to any web host and the link works the same way.

```
Open minishell.html  →  a full Ubuntu-style terminal
```

Run `selftest` inside it to check everything works on that machine: **63 checks, all passing.**

---

## Why it is written this way

The earlier version in `minishell-web/` was written in C and ran real programs through a
pseudo-terminal. That worked, but it only ran on macOS or Linux, it needed a compiler and a
server, and the output came from the host computer — on a Mac, `ls` and `ps` printed macOS
output, not Linux output.

This version carries its own Linux inside the page:

| Part | How it works here |
|---|---|
| The file system | A Linux directory tree (`/bin`, `/etc`, `/home/user` …) held in memory and saved in the browser, so your files are still there next time |
| The commands | Each one is written from scratch to produce **GNU/Linux** output, not the host computer's |
| The processes | A process table with real PIDs, process groups, states (R, S, T, Z) and signals |
| The C compiler | A complete lexer, parser and interpreter for C, so `gcc` and `./a.out` work with no compiler installed |
| The editor | A gedit window with C syntax colouring |

Nothing is borrowed from the computer it runs on, so the output is identical everywhere.

---

## What it can do

**Files and folders** — `pwd`, `cd`, `ls` (`-l -a -A -h -R -r -1`), `mkdir` (`-p`), `rmdir`,
`rm` (`-r -f`), `cp` (`-r`), `mv`, `touch`, `cat` (`-n`), `find`, `tree`, `chmod`

**Text** — `echo`, `head`, `tail`, `wc`, `grep` (`-i -n -c -v`), `sort` (`-n -r -u`), `uniq`,
`seq`, `yes`, `true`, `false`

**Programs** — `gedit`, `gcc` (`-o`, `-Wall`), `./a.out`, `man`, `help`, `demo`, `selftest`

**Processes and jobs** — `ps`, `ps aux`, `pid`, `kill` (`-9`, `-l`, `%job`), `jobs`, `fg`, `bg`,
`sleep`, **Ctrl-C**, **Ctrl-Z**, `&`

**System** — `whoami`, `hostname`, `id`, `uname`, `date`, `uptime`, `free`, `df`, `which`,
`env`, `export`, `unset`, `history`, `clear`, `exit`

**Shell features** — pipes `|`, redirects `>` `>>` `<` `2>` `2>>` `2>&1`, background `&`,
lists `;` `&&` `||`, wildcards `*` `?` `[a-z]`, variables `$HOME` `${VAR}` `$?` `$$`,
quoting `'` `"` `\`, comments `#`, history with `!!` and `!5`, Tab completion,
Up/Down history, Ctrl-A/E/U/K/W/L.

### The C it understands

`int char float double long short unsigned void` · 1-D and 2-D arrays · strings ·
`if/else` `for` `while` `do-while` `switch/case` `break` `continue` `return` ·
functions and recursion · all the usual operators including `++ -- += ?: % & | ^ << >>` ·
`printf` with widths, flags and precision (`%5d %-5d %05d %.2f %s %c %x %o %e`) ·
`scanf` · `puts` `putchar` `getchar` `gets` · `strlen` `strcpy` `strcmp` `strcat` ·
`sqrt` `pow` `abs` `floor` `ceil` `sin` `cos` `rand` · `sleep` `getpid` `exit`

Mistakes are reported the way gcc reports them:

```
hello.c:3:17: error: expected ';' before 'printf'
    3 |     int a = 1
      |                 ^
compilation terminated.
```

---

## A demo you can present

```bash
pwd ; ls -l                     # where am I, what is here
mkdir osda && cd osda           # make a folder and go into it
cat >> notes.txt                # type lines, finish with Ctrl-D
cat notes.txt ; mv notes.txt os.txt
gedit hello.c                   # write a C program, Ctrl+S, close the window
gcc hello.c                     # compile it
./a.out                         # run it: scanf waits for your input
ps ; ps aux ; pid               # the process table
sleep 30                        # then press Ctrl-Z
jobs ; pid %1 ; bg ; jobs ; fg  # job control, then press Ctrl-C
echo $?                         # 130 = killed by signal 2 (Ctrl-C)
ls -l | grep '^d' | wc -l       # pipes
ls *.c ; cat os.txt > copy.txt  # wildcards and redirection
man ls ; man kill               # manual pages
selftest                        # 63 automatic checks
```

Type `demo` inside the shell to see this list on screen.

---

## How the single file is organised

Every section is marked with a comment banner, and **every line has a comment**.

| Section | What it holds |
|---|---|
| 1–2 | Helpers, and the virtual file system |
| 3–4 | The screen (printing, the input line, history, Tab), processes and the job table |
| 5–6 | Reading the command line: quoting, variables, wildcards, pipes, redirects; and how a command prints and reads |
| 7–9 | Every command, and the manual pages |
| 10–13 | The C compiler: lexer, parser, interpreter, the C library, and `gcc` |
| 14–16 | Pipes, output destinations, and the gedit window |
| 17–19 | The shell itself, the self-test, and the start-up code |

---

## Known limits

- Programs that take over the whole screen (`vim`, `top`, `less`) are not included.
- `malloc`, `fork` and `system` inside C programs report that they are unavailable.
- Only the options listed above are supported; others are ignored rather than refused.
- Files are saved in the browser you opened it with, so a different browser starts fresh.
  Run `rm -r *` or clear the site data to start over.

---

## The older version

`minishell-web/` holds the earlier C version that runs a real shell through a pseudo-terminal.
It still works on macOS and Linux and is worth keeping as evidence of the system-call work
(`fork`, `execvp`, `pipe`, `dup2`, `waitpid`, `tcsetpgrp`, `kill`), but `minishell.html` is the
version to demonstrate, because it runs anywhere.
