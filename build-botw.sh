#!/bin/bash
# Breath of the Wild: by default only the game program and launcher are rebuilt and swapped into the app an earlier full
# build made (--quick). ./build-botw.sh --full does the whole build. Other arguments go to build_app.py.
# BOTW_GAME / BOTW_KEYS / BOTW_WORK override the paths below.
set -euo pipefail
cd "$(dirname "$0")"

GAME="${BOTW_GAME:-/Volumes/WD/Downloads/The Legend of Zelda Breath of the Wild [01007EF00011E000]+[v1.6.0+2DLC][US](nsw2u.com).xci}"
KEYS="${BOTW_KEYS:-$HOME/Library/Application Support/Ryujinx/system/prod.keys}"
WORK="${BOTW_WORK:-/Volumes/WD/BotW_build}"

mode=(--quick)
args=()
for a in "$@"; do
  if [ "$a" = "--full" ]; then mode=(); else args+=("$a"); fi
done

python3.14 build_app.py "$GAME" \
  -k "$KEYS" \
  --work "$WORK" \
  -o "$WORK/apps/Breath of the Wild.app" ${mode[@]+"${mode[@]}"} ${args[@]+"${args[@]}"}
