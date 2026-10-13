#!/bin/sh
# Placeholder "exam server" so that `exam start/stop/status/logs` work today.
# Later you replace it with the real Java server by setting:
#   export MEDSH_EXAM_CMD="java -jar exam-server/target/exam-server.jar"

trap 'echo "[server] SIGTERM received, shutting down"; exit 0' TERM INT
echo "[server] started, pid $$"
i=0
while true; do
    i=$((i + 1))
    echo "[server] heartbeat $i"
    sleep 1 &
    wait $!
done
