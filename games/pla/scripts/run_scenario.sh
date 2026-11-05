#!/usr/bin/env bash
# run_scenario.sh — one reproducible PLA run, for regression baselines and A/B timing.
#
# Runs the already-built default binary for a fixed wall-clock duration, with the
# scripted "mash A" input when it is available, then terminates it cleanly
# (SIGTERM -> grace -> SIGKILL). stderr goes to <log-dir>/<tag>.log, stdout to
# <log-dir>/<tag>.out, and the raw wait status to <log-dir>/<tag>.rc.
#
# This script never builds anything. Build first (make runtime), then run:
#
#   scripts/run_scenario.sh --tag field-$(date +%m%d-%H%M) --seconds 120
#   scripts/run_scenario.sh --tag menu-1 --seconds 60 --env TOTK_FPS_OVERLAY=1
#   scripts/run_scenario.sh --dry-run --tag smoke
#
# Defaults (all overridable):
#   build dir  $PLA_BUILD_DIR, else <repo>/PLA_build
#   log dir    $PLA_LOG_DIR,   else /tmp/pla_re
#   binary     ./build/pla-native rewritten/v1101 boot   (run inside the build dir)
#   seconds    60
#   auto input on when an inp_auto.txt is found (TOTK_INPUT_FILE + TOTK_ASSUME_FOCUS=1)
#
# Auto input search order: $PLA_INPUT_FILE, <log-dir>/inp_auto.txt,
# /tmp/pla_re/inp_auto.txt. Disable with --no-auto-input.
#
# Exit codes:
#   0  ran for the requested duration (we terminated it; see <tag>.rc for the raw status)
#   2  usage error
#   3  process died before the duration elapsed (crash/abort/early quit; log tail printed)
#   4  build dir / binary / game data missing (or log dir not writable)
#   130 interrupted by the user (SIGINT/SIGTERM/SIGHUP)
#
# The tag is used as a filename, so only [A-Za-z0-9._-] is accepted.

set -u

SELF_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_DIR=$(cd "$SELF_DIR/.." && pwd)

TAG="run-$(date +%Y%m%d-%H%M%S)"
SECONDS_TO_RUN=60
GRACE=5
BUILD_DIR="${PLA_BUILD_DIR:-$REPO_DIR/PLA_build}"
LOG_DIR="${PLA_LOG_DIR:-/tmp/pla_re}"
BIN="./build/pla-native"
AUTO_INPUT=1
DRY_RUN=0
AUTO_INPUT_FILE=""
declare -a BIN_ARGS=("rewritten/v1101" "boot")
declare -a USER_ENV=()

usage() {
  local code="${1:-2}"
  cat <<'EOF'
usage: run_scenario.sh [options] [ENV=VALUE ...]

  --tag NAME         log tag, default run-YYYYmmdd-HHMMSS (only [A-Za-z0-9._-])
  --seconds N        wall-clock duration to keep the game running, default 60
  --grace N          seconds between SIGTERM and SIGKILL, default 5
  --build-dir DIR    build folder (holds build/pla-native and rewritten/),
                     default $PLA_BUILD_DIR or <repo>/PLA_build
  --log-dir DIR      where <tag>.log/.out/.rc/.cmd are written,
                     default $PLA_LOG_DIR or /tmp/pla_re
  --binary PATH      binary, relative to --build-dir or absolute,
                     default ./build/pla-native
  --input-file FILE  scripted input file (auto-detected by default)
  --no-auto-input    do not inject TOTK_INPUT_FILE / TOTK_ASSUME_FOCUS
  --env NAME=VALUE   extra environment (repeatable)
  --dry-run          print the resolved command/paths and exit, launch nothing
  -h, --help         this text

Exit: 0 = full run (we terminated it), 3 = died early, 2 = usage, 4 = artifacts
missing, 130 = interrupted. Untouched on failure: the log is kept for triage.
EOF
  exit "$code"
}

die() {
  printf 'run_scenario: %s\n' "$*" >&2
  exit "${2:-2}"
}

warn() {
  printf 'run_scenario: %s\n' "$*" >&2
}

need_value() {
  [ $# -ge 2 ] && [ -n "$2" ] || die "option $1 needs a value"
}

# Parse args. Long options take the next argument; bare NAME=VALUE arguments are
# collected as extra environment. "--" makes everything after it an env pair.
while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)  usage 0 ;;
    --tag)      need_value "$@"; TAG="$2"; shift 2 ;;
    --seconds)  need_value "$@"; SECONDS_TO_RUN="$2"; shift 2 ;;
    --grace)    need_value "$@"; GRACE="$2"; shift 2 ;;
    --build-dir) need_value "$@"; BUILD_DIR="$2"; shift 2 ;;
    --log-dir)  need_value "$@"; LOG_DIR="$2"; shift 2 ;;
    --binary)   need_value "$@"; BIN="$2"; shift 2 ;;
    --input-file) need_value "$@"; AUTO_INPUT_FILE="$2"; shift 2 ;;
    --no-auto-input) AUTO_INPUT=0; shift ;;
    --env)      need_value "$@"; USER_ENV+=("$2"); shift 2 ;;
    --dry-run)  DRY_RUN=1; shift ;;
    --)         shift; while [ $# -gt 0 ]; do USER_ENV+=("$1"); shift; done ;;
    -*)         die "unknown option: $1" ;;
    *)
      case "$1" in
        [A-Za-z_][A-Za-z0-9_]*=*) USER_ENV+=("$1") ;;
        *) die "unexpected argument: $1 (tags/seconds are options; env is NAME=VALUE)" ;;
      esac
      shift ;;
  esac
done

# ---- validate -------------------------------------------------------------
case "$TAG" in
  ""|*[!A-Za-z0-9._-]*) die "--tag must be non-empty and only [A-Za-z0-9._-]: '$TAG'" ;;
esac
case "$SECONDS_TO_RUN" in
  ""|*[!0-9]*) die "--seconds must be a positive integer: '$SECONDS_TO_RUN'" ;;
esac
[ "$SECONDS_TO_RUN" -ge 1 ] || die "--seconds must be >= 1"
case "$GRACE" in
  ""|*[!0-9]*) die "--grace must be a non-negative integer: '$GRACE'" ;;
esac
for e in ${USER_ENV[@]+"${USER_ENV[@]}"}; do
  case "$e" in
    [A-Za-z_][A-Za-z0-9_]*=*) ;;
    *) die "bad env assignment '$e' (want NAME=VALUE)" ;;
  esac
done

if [ -d "$BUILD_DIR" ]; then
  BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
else
  case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$PWD/$BUILD_DIR" ;;
  esac
  # dry-run is a preview: report the missing folder instead of failing, so the
  # resolved paths/command can be inspected before the first build.
  [ "$DRY_RUN" = 1 ] || die "build dir not found: '$BUILD_DIR'" 4
fi
case "$BIN" in
  /*) BIN_PATH="$BIN" ;;
  ./*) BIN_PATH="$BUILD_DIR/${BIN#./}" ;;
  *)  BIN_PATH="$BUILD_DIR/$BIN" ;;
esac
LOG_PATH="$LOG_DIR/$TAG.log"
OUT_PATH="$LOG_DIR/$TAG.out"
RC_PATH="$LOG_DIR/$TAG.rc"
CMD_PATH="$LOG_DIR/$TAG.cmd"

# ---- auto input -----------------------------------------------------------
declare -a RUN_ENV=()
INPUT_FILE=""
if [ "$AUTO_INPUT" = 1 ]; then
  if [ -n "$AUTO_INPUT_FILE" ]; then
    INPUT_FILE="$AUTO_INPUT_FILE"
  elif [ -n "${PLA_INPUT_FILE:-}" ]; then
    INPUT_FILE="$PLA_INPUT_FILE"
  elif [ -f "$LOG_DIR/inp_auto.txt" ]; then
    INPUT_FILE="$LOG_DIR/inp_auto.txt"
  elif [ -f /tmp/pla_re/inp_auto.txt ]; then
    INPUT_FILE=/tmp/pla_re/inp_auto.txt
  fi
  if [ -n "$INPUT_FILE" ]; then
    if [ ! -r "$INPUT_FILE" ]; then
      warn "input file not readable: $INPUT_FILE (continuing without scripted input)"
      INPUT_FILE=""
    else
      RUN_ENV+=("TOTK_INPUT_FILE=$INPUT_FILE" "TOTK_ASSUME_FOCUS=1")
    fi
  fi
fi
RUN_ENV+=(${USER_ENV[@]+"${USER_ENV[@]}"})

# ---- dry run --------------------------------------------------------------
if [ "$DRY_RUN" = 1 ]; then
  printf 'run_scenario.sh — dry run (nothing launched)\n'
  printf '  repo      : %s\n' "$REPO_DIR"
  printf '  build dir : %s\n' "$BUILD_DIR"
  printf '  log dir   : %s\n' "$LOG_DIR"
  printf '  log       : %s\n' "$LOG_PATH"
  printf '  duration  : %s s, then SIGTERM and SIGKILL after %s s grace\n' "$SECONDS_TO_RUN" "$GRACE"
  if [ -n "$INPUT_FILE" ]; then
    printf '  auto input: %s (TOTK_INPUT_FILE, TOTK_ASSUME_FOCUS=1)\n' "$INPUT_FILE"
  else
    printf '  auto input: off\n'
  fi
  printf '  command   : cd %q &&' "$BUILD_DIR"
  if [ ${#RUN_ENV[@]} -gt 0 ]; then
    printf ' env'
    for e in ${RUN_ENV[@]+"${RUN_ENV[@]}"}; do printf ' %q' "$e"; done
  fi
  printf ' %q' "$BIN_PATH" "${BIN_ARGS[@]}"
  printf '\n'
  missing=0
  [ -d "$BUILD_DIR" ] || { printf '  WARN      : build dir missing\n'; missing=1; }
  [ -x "$BIN_PATH" ] || { printf '  WARN      : binary missing/not executable: %s\n' "$BIN_PATH"; missing=1; }
  [ -d "$BUILD_DIR/rewritten/v1101" ] || { printf '  WARN      : game data missing: %s/rewritten/v1101\n' "$BUILD_DIR"; missing=1; }
  [ "$missing" = 1 ] && printf '  note      : a real run would exit 4 until the build/data exist\n'
  exit 0
fi

# ---- preflight ------------------------------------------------------------
[ -x "$BIN_PATH" ] || die "binary missing or not executable: $BIN_PATH (run make runtime first)" 4
[ -d "$BUILD_DIR/rewritten/v1101" ] || die "game data missing: $BUILD_DIR/rewritten/v1101" 4
mkdir -p "$LOG_DIR" 2>/dev/null || die "cannot create log dir: $LOG_DIR" 4
[ -w "$LOG_DIR" ] || die "log dir not writable: $LOG_DIR" 4
# Make it absolute *before* cd, so a relative --log-dir keeps pointing where the
# caller meant it to.
LOG_DIR=$(cd "$LOG_DIR" && pwd) || die "cannot resolve log dir: $LOG_DIR" 4
LOG_PATH="$LOG_DIR/$TAG.log"
OUT_PATH="$LOG_DIR/$TAG.out"
RC_PATH="$LOG_DIR/$TAG.rc"
CMD_PATH="$LOG_DIR/$TAG.cmd"

# ---- launch ---------------------------------------------------------------
cd "$BUILD_DIR" || die "cannot cd to build dir: $BUILD_DIR" 4
: > "$LOG_PATH"
: > "$OUT_PATH"

{
  printf '# tag: %s\n' "$TAG"
  printf '# duration: %s s (grace %s s)\n' "$SECONDS_TO_RUN" "$GRACE"
  printf '# log: %s\n' "$LOG_PATH"
  printf 'cd %q &&' "$BUILD_DIR"
  if [ ${#RUN_ENV[@]} -gt 0 ]; then
    printf ' env'
    for e in ${RUN_ENV[@]+"${RUN_ENV[@]}"}; do printf ' %q' "$e"; done
  fi
  printf ' %q' "$BIN_PATH" "${BIN_ARGS[@]}"
  printf '\n'
} > "$CMD_PATH"

if [ ${#RUN_ENV[@]} -gt 0 ]; then
  ( exec env ${RUN_ENV[@]+"${RUN_ENV[@]}"} "$BIN_PATH" "${BIN_ARGS[@]}" ) >"$OUT_PATH" 2>"$LOG_PATH" &
else
  ( exec "$BIN_PATH" "${BIN_ARGS[@]}" ) >"$OUT_PATH" 2>"$LOG_PATH" &
fi
PID=$!

alive() {
  kill -0 "$1" 2>/dev/null || return 1
  local st
  st=$(ps -o stat= -p "$1" 2>/dev/null | tr -d ' ') || return 1
  [ -n "$st" ] && [ "${st#Z}" = "$st" ]
}

INTERRUPTED=0
on_signal() {
  INTERRUPTED=1
}
trap on_signal INT TERM HUP

# Terminate the game: SIGTERM, wait up to $GRACE seconds, then SIGKILL.
terminate() {
  kill -TERM "$PID" 2>/dev/null || true
  local until=$((SECONDS + GRACE))
  while alive "$PID" && [ "$SECONDS" -lt "$until" ]; do sleep 0.2; done
  if alive "$PID"; then
    warn "still alive after ${GRACE}s grace; sending SIGKILL"
    kill -KILL "$PID" 2>/dev/null || true
  fi
}

printf 'run_scenario: %s → %s (%s s)\n' "$TAG" "$LOG_PATH" "$SECONDS_TO_RUN"
START=$SECONDS

DEADLINE=$((SECONDS + SECONDS_TO_RUN))
REACHED=0
while alive "$PID"; do
  if [ "$INTERRUPTED" = 1 ]; then break; fi
  if [ "$SECONDS" -ge "$DEADLINE" ]; then
    REACHED=1
    break
  fi
  sleep 0.2
done
# Either the deadline passed, the user interrupted us, or the process exited by itself.
if [ "$REACHED" = 1 ] || [ "$INTERRUPTED" = 1 ]; then
  terminate
fi

wait "$PID" 2>/dev/null
RC=$?
trap - INT TERM HUP
# It can exit with 0 in the same instant the deadline fires; do not report that
# as an early death.
if [ "$REACHED" = 0 ] && [ "$INTERRUPTED" = 0 ] && [ "$RC" -eq 0 ] \
   && [ "$((SECONDS - START))" -ge "$SECONDS_TO_RUN" ]; then
  REACHED=1
fi
printf '%s\n' "$RC" > "$RC_PATH"

# LC_ALL=C: the frame counter pattern is multibyte; byte-wise matching is what we want here.
frames=$(LC_ALL=C grep -a -o '至帧 [0-9][0-9]*' "$LOG_PATH" 2>/dev/null | tail -1 | awk '{print $2}')
frames=${frames:-0}
first=""
if LC_ALL=C grep -aq 'first presented frame' "$LOG_PATH" 2>/dev/null; then
  first=$(LC_ALL=C grep -a -m1 -o '\[totk-boot\] [0-9.]* first presented frame' "$LOG_PATH" | awk '{print $2}')
fi

if [ "$INTERRUPTED" = 1 ]; then
  printf 'run_scenario: interrupted; raw status %s, frames %s, log %s\n' "$RC" "$frames" "$LOG_PATH"
  exit 130
fi
if [ "$REACHED" = 1 ]; then
  printf 'run_scenario: done; raw status %s, frames %s%s, log %s (rc file %s)\n' \
    "$RC" "$frames" "${first:+, first frame ${first}s}" "$LOG_PATH" "$RC_PATH"
  exit 0
fi

printf 'run_scenario: process exited after %ss, before the %ss duration (raw status %s)\n' \
  "$((SECONDS - START))" "$SECONDS_TO_RUN" "$RC" >&2
printf 'run_scenario: last frames line: 至帧 %s; log tail:\n' "$frames" >&2
tail -n 15 "$LOG_PATH" >&2 || true
exit 3
