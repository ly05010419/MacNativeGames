#!/bin/bash
# Build Game.command: double-click this file in Finder to open the MacNativeGames builder window.
# It finds Python 3.14, checks that it has Tk when no arguments are given, and runs
# `python3.14 -m tools.builder` from this folder. Arguments typed after it in a terminal pass through,
# for example: "./Build Game.command" --cli --game totk --game-file <game.xci> --keys <prod.keys> --check-only
# Set BUILDER_PYTHON to use another Python 3.14. This script only reads: it installs and writes nothing.
set -u

wait_and_exit() {
    read -r -p "Press Return to close this window. " _ || true
    exit 1
}

is_program() {
    [ -n "$1" ] && [ -f "$1" ] && [ -x "$1" ]
}

PY=""
if [ -n "${BUILDER_PYTHON:-}" ]; then
    if ! is_program "$BUILDER_PYTHON"; then
        echo "BUILDER_PYTHON is not executable: $BUILDER_PYTHON"
        wait_and_exit
    fi
    PY="$BUILDER_PYTHON"
else
    for candidate in /opt/homebrew/bin/python3.14 /usr/local/bin/python3.14 "$(command -v python3.14 || true)"; do
        if is_program "$candidate"; then
            PY="$candidate"
            break
        fi
    done
fi
if [ -z "$PY" ]; then
    echo "Python 3.14 is needed: brew install python@3.14"
    wait_and_exit
fi

cd "$(dirname "$0")" || wait_and_exit

if [ "$#" -eq 0 ] && ! "$PY" -c 'import tkinter' 2>/dev/null; then
    echo "The window needs Tk: brew install python-tk@3.14"
    echo "Or use the text mode from a terminal, for example:"
    echo "  \"$PY\" -m tools.builder --cli --game totk --game-file <game.xci> --keys <prod.keys>"
    wait_and_exit
fi

exec "$PY" -m tools.builder "$@"
