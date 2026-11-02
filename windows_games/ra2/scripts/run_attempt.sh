#!/usr/bin/env bash
# Run the recompiled game once with a watchdog and keep its output.
#
#   scripts/run_attempt.sh <name> [seconds] [game arguments...]
#
# Output goes to game/attempts/<name>.log (git-ignored). The process is
# killed after <seconds> (default 40) if it is still running.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NAME="${1:?usage: run_attempt.sh <name> [seconds] [args...]}"
SECS="${2:-40}"
shift $(( $# >= 2 ? 2 : 1 ))
LOG="$ROOT/game/attempts/$NAME.log"
mkdir -p "$ROOT/game/attempts"

"${RA2_BIN:-$ROOT/build/main/ra2}" "$@" > "$LOG" 2>&1 &
pid=$!
for _ in $(seq 1 "$SECS"); do
  kill -0 "$pid" 2>/dev/null || break
  sleep 1
done
if kill -0 "$pid" 2>/dev/null; then
  kill "$pid"
  wait "$pid" 2>/dev/null || true
  echo "run_attempt: still running after ${SECS}s, killed" >> "$LOG"
else
  status=0
  wait "$pid" || status=$?
  echo "run_attempt: exit $status" >> "$LOG"
fi
echo "$LOG"
