#!/bin/zsh
# Dev boot: scripts/dev/boot.sh <log name> [seconds=60]. Opens the game window (HEADLESS=1 for none), stops after
# the time limit (SAMPLE=<s>: a macOS sample report at that second, <log>.sample.txt), prints the log tail after nnMain with symbolized guest callers. Logs: work/logs (WORK in the Makefile).
cd "${0:A:h}/../.." || exit 1
L=work; n=$1; t=${2:-60}; mkdir -p $L/logs $L/profile $L/shader-cache
make runtime 2>&1 | grep error: && exit 1
(env ${HEADLESS:+TOTK_HEADLESS=1} TOTK_SLOT_OWNER=claude ${=EXTRA} ${SHOT:+TOTK_WINDOW_SHOT=$SHOT} TOTK_PROFILE_DIR=$L/profile TOTK_ROMFS_PATCHES=0 TOTK_GPU_ALLOW_SUBMIT=1 TOTK_GPU_WIDEN=1 TOTK_GPU_BLOCKRT=1 TOTK_GPU_MIDCLEAR=1 TOTK_GPU_BUDGET_MB=8000 TOTK_SHADER_CACHE=$L/shader-cache build/lm3-native rewritten/v120 boot > $L/logs/$n.log 2>&1 & P=$!
 for i in $(seq $t); do sleep 1; kill -0 $P 2>/dev/null || break; [ "$i" = "${SAMPLE:-0}" ] && sample $P 2 -file $L/logs/$n.sample.txt >/dev/null 2>&1; done; kill $P 2>/dev/null)
grep caller $L/logs/$n.log | .venv/bin/python scripts/dev/sym.py | cut -c1-230 | tail -${CALLERS:-12}
awk '/ENTER nnMain/{f=1} f' $L/logs/$n.log | grep -v "^x=\|caller" | tail -${TAIL:-20} | cut -c1-300
