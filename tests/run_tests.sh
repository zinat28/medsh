#!/bin/bash
# Run from the medsh folder:  make test
# Feeds commands to ./medsh through stdin and checks the output.
# (Ctrl-C / Ctrl-Z / fg / bg need a real terminal, so test those by hand.)

cd "$(dirname "$0")/.." || exit 1
SH=./medsh
pass=0; fail=0

check() {   # check "name" "commands" "expected output"
    local name="$1" input="$2" expected="$3" got
    got=$(printf '%s\n' "$input" | $SH 2>&1)
    if [ "$got" == "$expected" ]; then
        echo "PASS  $name"; pass=$((pass+1))
    else
        echo "FAIL  $name"; echo "  expected: $(printf %q "$expected")"; echo "  got     : $(printf %q "$got")"
        fail=$((fail+1))
    fi
}

rm -f /tmp/medsh_t1.txt
check "simple command"        "echo hello world"                     "hello world"
check "quotes"                "echo 'a   b'  \"c   d\""              "a   b c   d"
check "pipe"                  "echo hello | tr a-z A-Z"              "HELLO"
check "3-stage pipe"          "printf 'b\na\nc\n' | sort | head -n 2" $'a\nb'
check "output redirect"       "echo hi > /tmp/medsh_t1.txt
cat /tmp/medsh_t1.txt"        "hi"
check "append redirect"       "echo one > /tmp/medsh_t1.txt
echo two >> /tmp/medsh_t1.txt
cat /tmp/medsh_t1.txt"        $'one\ntwo'
check "input redirect"        "wc -l < /tmp/medsh_t1.txt"            "2"
check "semicolon"             "echo a; echo b"                       $'a\nb'
check "and (true)"            "true && echo yes"                     "yes"
check "and (false)"           "false && echo yes"                    ""
check "or"                    "false || echo fallback"               "fallback"
check "cd + pwd"              "cd /tmp
pwd"                          "/tmp"
check "builtin redirect"      "pwd > /tmp/medsh_t1.txt
cat /tmp/medsh_t1.txt"        "$(pwd)"
check "builtin in pipe"       "history | wc -l"                      "1"
check "command not found"     "nosuchcmd_xyz"                        "medsh: nosuchcmd_xyz: command not found"
check "exit status via &&"    "sh -c 'exit 3' || echo failed"        "failed"
check "exit builtin"          "exit 5
echo never"                   ""
check "syntax: lone pipe"     "| ls"                                 "medsh: syntax error near '|'"
check "syntax: no file"       "echo hi >"                            "medsh: syntax error: missing file after '>'"
check "syntax: quote"         "echo 'oops"                           "medsh: syntax error: unterminated quote"
out=$(printf 'sleep 0.2 &\necho after\n' | $SH 2>&1)
if echo "$out" | grep -qE '^\[1\] [0-9]+$' && echo "$out" | grep -q '^after$'; then
    echo "PASS  background job"; pass=$((pass+1))
else
    echo "FAIL  background job"; echo "$out"; fail=$((fail+1))
fi

# exam commands
export MEDSH_HOME="$(pwd)"
rm -rf run
check "exam status stopped"   "exam status"                          "exam server: STOPPED"
out=$(printf 'exam start\nexam status\nexam stop\nexam status\n' | $SH 2>&1)
if echo "$out" | grep -q "started" && echo "$out" | grep -q "RUNNING" && echo "$out" | grep -q "stopped" && echo "$out" | tail -n1 | grep -q STOPPED; then
    echo "PASS  exam start/status/stop"; pass=$((pass+1))
else
    echo "FAIL  exam start/status/stop"; echo "$out"; fail=$((fail+1))
fi

echo "-----"; echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
