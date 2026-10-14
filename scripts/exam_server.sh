#!/bin/sh
# Starts the Java exam server. medsh runs this from `exam start`.
# It compiles the server the first time (or when the .java file changed).

cd "$(dirname "$0")/.." || exit 1

if ! command -v javac >/dev/null 2>&1; then
    echo "[server] javac not found. Install it with: sudo apt install default-jdk"
    exit 1
fi

if [ ! -f server/classes/ExamServer.class ] || [ server/ExamServer.java -nt server/classes/ExamServer.class ]; then
    echo "[server] compiling ExamServer.java ..."
    mkdir -p server/classes
    javac -d server/classes server/ExamServer.java || exit 1
fi

# exec = replace this shell by java, so signals (SIGTERM from `exam stop`) reach java directly
exec java -cp server/classes ExamServer "${MEDSH_EXAM_PORT:-5050}"
