#!/usr/bin/env python3
"""End-to-end tests: runs ./minishell inside a real pseudo-terminal and types
commands exactly like a user would (including Ctrl-C, Ctrl-Z and Ctrl-D)."""
import os
import pty
import re
import select
import shutil
import signal
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHELL = os.path.join(ROOT, "minishell")
ANSI = re.compile(r"\x1b\][^\x07]*\x07|\x1b\[[0-9;?]*[A-Za-z]|\r|[\x08]")

work = tempfile.mkdtemp(prefix="minishell-test-")
pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.execve(SHELL, ["minishell"], dict(os.environ, MINISHELL_WEB="1", TERM="xterm"))

def read(timeout):
    out, end = b"", time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                out += os.read(fd, 65536)
            except OSError:
                break
    return out.decode(errors="replace")

def type_(text, wait=0.6):
    os.write(fd, text.encode())
    return read(wait)

passed = failed = 0
def check(name, cond, output=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"  \033[32mPASS\033[0m {name}")
    else:
        failed += 1
        print(f"  \033[31mFAIL\033[0m {name}\n{ANSI.sub('', output)[-600:]}")

read(1.0)

print("== Files and directories")
type_("mkdir demo\r")
check("mkdir + cd + pwd", "/demo" in type_("cd demo\r") + type_("pwd\r"))
type_("cat >> a.txt\r", 0.4)
type_("first line\r", 0.3)
type_("\x04", 0.5)                                   # Ctrl-D ends cat
out = type_("cat a.txt\r")
check("cat >> file, Ctrl-D", "first line" in out, out)
type_("mv a.txt b.txt\r")
out = type_("ls\r")
check("mv + ls", "b.txt" in out and "a.txt" not in out.split("ls", 1)[-1], out)

print("== Processes")
out = type_("ps\r", 1.0)
check("ps", "PID" in out, out)
out = type_("ps -aux\r", 1.5)
check("ps -aux", "USER" in out and "PID" in out, out)
out = type_("pid\r")
check("pid", "PID  (this shell)" in out and "PGID" in out, out)
out = type_("man pid\r", 1.0)
check("man (built-in page)", "NAME" in out and "pid - show process IDs" in out, out)
out = type_("man mkdir\r", 3.0)
plain = re.sub(r".\x08", "", out)                    # man bolds text by overstriking
check("man (system page)", "NAME" in plain and "mkdir" in plain, out)
out = type_("rm b.txt\r")
check("unsupported command rejected", "command not supported" in out, out)

print("== gcc and ./a.out")
program = ('#include <stdio.h>\rint main(){int n;printf("Enter a number: ");'
           'scanf("%d",&n);printf("Square of %d is %d\\n",n,n*n);return 0;}\r')
type_("cat >> h.c\r", 0.4)
type_(program, 0.4)
type_("\x04", 0.5)
out = type_("gcc h.c\r", 6.0)
check("gcc h.c", "error" not in out.lower(), out)
out = type_("./a.out\r", 1.0)
check("./a.out prompt appears before input (line-buffered TTY)", "Enter a number:" in out, out)
out = type_("7\r", 1.0)
check("./a.out reads stdin", "Square of 7 is 49" in out, out)

print("== Ctrl-C, Ctrl-Z, jobs, fg, bg, kill")
type_("cat\r", 0.5)
out = type_("\x03", 0.6)
check("Ctrl-C interrupts the program", "^C" in out, out)
type_("cat\r", 0.5)
out = type_("\x1a", 0.8)
check("Ctrl-Z stops the program", "Stopped" in out and "cat" in out, out)
out = type_("jobs\r")
check("jobs lists the stopped job", "[1]+" in out and "Stopped" in out, out)
out = type_("pid %1\r")
check("pid %1 shows the job's process", "stopped" in out, out)
out = type_("fg\r", 0.8)
check("fg resumes the job in the foreground", "cat" in out, out)
if "Interrupted system call" in out:
    # macOS: a read() interrupted by Ctrl-Z fails with EINTR once resumed
    # (bash and zsh on macOS behave the same). Linux keeps reading.
    print("         (macOS: cat's read() returned EINTR after resume, same as in bash/zsh)")
else:
    type_("resumed\r", 0.5)
    out = type_("\x04", 0.8)                          # Ctrl-D -> cat exits
    check("resumed job still reads the terminal", "resumed" in out, out)

type_("gedit notes.txt\r", 0.8)
out = type_("\x1a", 0.8)
check("gedit can be stopped with Ctrl-Z", "Stopped" in out and "gedit notes.txt" in out, out)
out = type_("bg\r", 0.6)
check("bg continues it in the background", "gedit notes.txt &" in out, out)
out = type_("kill %1\r", 0.6) + type_("\r", 0.6)
check("kill %1 terminates it", "Terminated" in out, out)

out = type_("gedit h.c\r", 0.8)
m = re.search(r"\x1b\]7777;gedit;(\d+);", out)
check("gedit announces its editor window", m is not None, out)
if m:
    os.kill(int(m.group(1)), signal.SIGUSR1)          # what closing the window does
    out = read(0.8)
    check("closing the window returns the prompt", "$ " in ANSI.sub("", out), out)

type_("exit\r", 0.8)
os.close(fd)
os.waitpid(pid, 0)
shutil.rmtree(work, ignore_errors=True)

print(f"\nPassed: {passed}   Failed: {failed}")
sys.exit(1 if failed else 0)
