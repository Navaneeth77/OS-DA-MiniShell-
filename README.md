# MiniShell++

A Linux terminal that runs completely inside a web browser — **one HTML file, nothing installed.**

Open [`index.html`](index.html) and you get an Ubuntu-style terminal with its own file system,
its own process table, its own C compiler and a gedit window. It works on Windows, macOS,
Linux, a phone and a college lab machine, online or offline, because nothing is borrowed from
the computer it runs on.

```
ls -l | grep '^d'        gedit hello.c        sleep 30  →  Ctrl-Z  →  jobs  →  bg  →  fg
mkdir osda && cd osda    gcc hello.c          ps aux    |  grep bash
cat >> notes.txt         ./a.out              man kill
```

Type `help` for every command, `demo` for a guided tour, and `selftest` to run the built-in
test suite: **63 checks, all passing.**

---

## Run it

| How | What to do |
|---|---|
| On your computer | Download `index.html` and double-click it |
| From this repository | `git clone` it and open `index.html` |
| On the web | Deploy to Vercel (below) and share the link |

No server, no build step, no internet connection needed.

### Deploy to Vercel

The repository is already set up, so there is nothing to configure:

1. Go to [vercel.com/new](https://vercel.com/new) and import `Navaneeth77/OS-DA-MiniShell-`.
2. Leave every setting as it is — the framework is "Other" and there is no build command.
3. Press **Deploy**.

`vercel.json` turns off the build step and serves the repository root, so `/` loads the
terminal straight away. To deploy from a terminal instead:

```bash
npm i -g vercel
vercel --prod
```

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

**Shell features** — pipes `|`, redirection `>` `>>` `<` `2>` `2>>` `2>&1`, background `&`,
command lists `;` `&&` `||`, wildcards `*` `?` `[a-z]`, variables `$HOME` `${VAR}` `$?` `$$`,
quoting `'` `"` `\`, comments `#`, history with `!!` and `!5`, Tab completion, Up/Down
history and the usual Ctrl-A/E/U/K/W/L editing keys.

### The C it compiles

`int char float double long short unsigned void` · arrays (1-D and 2-D) · strings ·
`if/else` `for` `while` `do-while` `switch/case` `break` `continue` `return` ·
functions and recursion · `++ -- += ?: % & | ^ << >>` ·
`printf` with widths, flags and precision (`%5d %-5d %05d %.2f %s %c %x %o %e`) · `scanf` ·
`puts` `putchar` `getchar` `gets` · `strlen` `strcpy` `strcmp` `strcat` ·
`sqrt` `pow` `abs` `floor` `ceil` `sin` `cos` `rand` · `sleep` `getpid` `exit`

Mistakes are reported the way gcc reports them:

```
hello.c:3:17: error: expected ';' before 'printf'
    3 |     int a = 1
      |                 ^
compilation terminated.
```

---

## How it is built

`index.html` is one file of about 5,600 commented lines. Every section is marked with a
banner comment, and every line of code carries a comment explaining it.

| Section | What it holds |
|---|---|
| 1–2 | Helpers, and the virtual file system (`/bin`, `/etc`, `/home/user`, saved in the browser) |
| 3–4 | The screen (printing, the input line, history, Tab) · processes and the job table |
| 5–6 | Reading the command line: quoting, variables, wildcards, pipes, redirection |
| 7–9 | Every command, and the manual pages |
| 10–13 | The C compiler: lexer, parser, interpreter, the C library, and `gcc` |
| 14–16 | Pipes between commands, where output goes, and the gedit window |
| 17–19 | The shell itself, the self-test, and the start-up code |

---

## Repository layout

```
index.html                  the project: the whole terminal, in one file
vercel.json                 hosting settings (no build step, serve the root)
MiniShell_DA2.pptx          the presentation
docs/
  notes-browser-version.md  notes on this version, with a demo script
  cpp-terminal-version.md   notes on the first version, written in C++
  MiniShell_DA2_Report.html the written report
minishell-web/              an earlier version in C: a real shell behind a pseudo-terminal
src/ include/ tests/        the first version, written in C++ (build it with `make`)
```

The two earlier versions are kept because they show the operating-system work directly:
`fork`, `execvp`, `pipe`, `dup2`, `waitpid`, `tcsetpgrp` and `kill`. They only run on macOS or
Linux. `index.html` is the version to demonstrate, because it runs anywhere.

---

## Known limits

- Programs that take over the whole screen (`vim`, `top`, `less`) are not included.
- `malloc`, `fork` and `system` inside a C program report that they are unavailable.
- Only the options listed above are supported; others are ignored rather than refused.
- Your files are saved in the browser you opened it in, so another browser starts fresh.

---

Built for an Operating Systems course assignment.
