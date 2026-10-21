#!/usr/bin/env bash
# Automated tests for MiniShell++ (non-interactive mode).
cd "$(dirname "$0")/.." || exit 1
SH=./minishell
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
pass=0; fail=0

check() {   # check "name" "expected" "actual"
    if [[ "$2" == "$3" ]]; then
        pass=$((pass + 1)); printf '  \033[32mPASS\033[0m %s\n' "$1"
    else
        fail=$((fail + 1)); printf '  \033[31mFAIL\033[0m %s\n       expected: %q\n       actual:   %q\n' "$1" "$2" "$3"
    fi
}
run() { "$SH" -c "$1" 2>&1; }
script() { printf '%s\n' "$1" | "$SH" 2>&1; }

echo "== External commands & built-ins"
check "simple command"        "hello world"       "$(run 'echo hello world')"
check "external with args"    "3"                 "$(run '/bin/echo a b c | wc -w | tr -d " "')"
check "pwd"                   "$(cd "$TMP" && pwd -P)"              "$(cd "$TMP" && "$OLDPWD/$SH" -c pwd)"
check "cd then pwd"           "/tmp"              "$(script $'cd /tmp\npwd' | sed 's#/private##')"
check "cd -"                  "/"                 "$(script $'cd /\ncd /tmp\ncd - >/dev/null\npwd')"
check "command not found"     "minishell: nosuchcmd: command not found|127" "$(script $'nosuchcmd\necho "|$?"' | tr -d '\n')"
check "exit status \$?"       $'1\n0'            "$(script $'false\necho $?\ntrue\necho $?')"
check "exit code"             "7"                 "$(run 'exit 7'; echo $?)"
check "type builtin"          "cd is a shell builtin" "$(run 'type cd')"

echo "== Quoting & expansion"
check "single quotes"         'a  $HOME b'        "$(run "echo 'a  \$HOME b'")"
check "double quotes + var"   "home=$HOME"        "$(run 'echo "home=$HOME"')"
check "braces"                "${HOME}x"          "$(run 'echo ${HOME}x')"
check "backslash escape"      'a b$'              "$(run 'echo a\ b\$')"
check "tilde"                 "$HOME/x"           "$(run 'echo ~/x')"
check "export / unset"        "bar|"              "$(script $'export FOO=bar\necho $FOO\nunset FOO\necho "|$FOO"' | tr -d '\n')"
check "comment"               "hi"                "$(run 'echo hi # ignored')"

echo "== Redirection"
run "echo first > $TMP/out.txt" >/dev/null
check "> truncate"            "first"             "$(cat "$TMP/out.txt")"
run "echo second >> $TMP/out.txt" >/dev/null
check ">> append"             $'first\nsecond'    "$(cat "$TMP/out.txt")"
check "< input"               "2"                 "$(run "wc -l < $TMP/out.txt" | tr -d ' ')"
run "ls /nonexistent 2> $TMP/err.txt" >/dev/null
check "2> stderr"             "1"                 "$(grep -c nonexistent "$TMP/err.txt")"
check "2>&1"                  "1"                 "$(run 'ls /nonexistent 2>&1 | grep -c nonexistent')"
check "builtin redirect"      "saved"             "$(run "echo saved > $TMP/b.txt"; cat "$TMP/b.txt")"
check "missing input file"    "minishell: $TMP/none: No such file or directory" "$(run "cat < $TMP/none")"

echo "== Pipes"
check "two-stage pipe"        "HELLO"             "$(run 'echo hello | tr a-z A-Z')"
check "multi-stage pipe"      "3"                 "$(run 'printf "c\nb\na\nb\nc\na\n" | sort | uniq | wc -l' | tr -d ' ')"
check "pipe + redirect"       "B"                 "$(run "printf 'b\na\n' | sort -r | head -1 | tr a-z A-Z > $TMP/p.txt"; cat "$TMP/p.txt")"
check "builtin in pipeline"   "X"                 "$(run 'echo x | tr x X')"
check "pipe exit status"      "0"                 "$(script $'false | true\necho $?')"

echo "== Background jobs"
check "background + wait"     $'start\ndone'      "$(script $'sleep 0.2 &\necho start\nwait\necho done')"
check "jobs listing"          "Running"           "$(script $'sleep 1 &\njobs' | grep -o Running)"
check "kill %job"             "0"                 "$(script $'sleep 5 &\nkill %1\nwait\njobs' | wc -l | tr -d ' ')"

echo "== History"
check "history + !!"          $'one\necho one\none' "$(script $'echo one\n!!')"
check "!N"                    $'two\necho two\ntwo' "$(script $'echo two\n!1')"

echo "== Syntax errors"
check "leading pipe"          "minishell: syntax error near unexpected token \`|'" "$(run '| ls')"
check "missing file"          "minishell: syntax error near unexpected token \`newline'" "$(run 'ls >')"
check "unclosed quote"        "minishell: unexpected EOF while looking for matching \`\"'" "$(run 'echo "abc')"

echo
echo "Passed: $pass   Failed: $fail"
[[ $fail -eq 0 ]]
