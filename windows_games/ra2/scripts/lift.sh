#!/usr/bin/env bash
# Lift GAME.EXE and Blowfish.dll into game/generated/ra2 (or, with
# LIFT_TARGET=yr, Yuri's Revenge's gamemd.exe into game/generated/yr).
#
#   [LIFT_TARGET=yr] scripts/lift.sh [install dir]   # default: game/install
#
# Inputs per module, in <generated dir>/symbols/<module>/: functions.tsv and
# jumptables.tsv (config/symbols/<module>/: Ghidra's lists for the Steam
# executables, addresses and flags only; SYMBOLS_DIR=<dir> for your own
# export of another version), exports.tsv and imports.tsv (pe_symbols.py
# from the player's own binary). Manifests are
# config/<module>.manifest. The output is written to a scratch directory
# and only files whose contents changed replace the old ones, so a
# rebuild recompiles only what the lift changed. The output is generated
# from the player's own files: git-ignored, never committed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL="${1:-$ROOT/game/install}"
# LIFT_TARGET: ra2 (Red Alert 2, GAME.EXE; the default) or yr (Yuri's
# Revenge, gamemd.exe), into game/generated/<target>.
TARGET="${LIFT_TARGET:-ra2}"
case "$TARGET" in
  ra2) MODULES=("GAME.EXE:game" "Blowfish.dll:blowfish") ;;
  yr) MODULES=("gamemd.exe:yr" "Blowfish.dll:blowfish") ;;
  *) echo "lift: unknown LIFT_TARGET $TARGET (ra2 or yr)" >&2; exit 2 ;;
esac
# GENERATED_DIR: where the recompiled code goes (default game/generated);
# LIFTER: x86recomp's recomp_lift (default the one scripts/build.sh builds).
GEN="${GENERATED_DIR:-$ROOT/game/generated}"
OUT="$GEN/$TARGET"
SCRATCH="$GEN/.$TARGET-new"
LIFT="${LIFTER:-$ROOT/build/main/x86recomp/tools/lifter/recomp_lift}"
[ -x "$LIFT" ] || { echo "lift: no lifter at $LIFT (scripts/build.sh builds it)" >&2; exit 1; }
PYTHON=python3
# The function and jump-table lists: the repository's, for the executables
# config/symbols/sha256.txt names, unless SYMBOLS_DIR names your own.
SYMBOLS="${SYMBOLS_DIR:-$ROOT/config/symbols}"

for i in "${!MODULES[@]}"; do
  file="${MODULES[$i]%%:*}" name="${MODULES[$i]##*:}"
  [ -f "$INSTALL/$file" ] || { echo "lift: $INSTALL/$file not found" >&2; exit 1; }
  [ -n "${SYMBOLS_DIR:-}" ] && continue
  want="$(awk -F'\t' -v m="$name" '$1 == m { print $3 }' "$ROOT/config/symbols/sha256.txt")"
  have="$(shasum -a 256 "$INSTALL/$file" | cut -d' ' -f1)"
  if [ "$have" != "$want" ]; then
    echo "lift: $file is not the version the repository's lists are for (SHA-256 $have, want $want)." >&2
    echo "      Export your own with Ghidra and pass SYMBOLS_DIR (install.md: Another version)." >&2
    exit 1
  fi
done

args=()
for i in "${!MODULES[@]}"; do
  file="${MODULES[$i]%%:*}" name="${MODULES[$i]##*:}"
  dir="$GEN/symbols/$name"
  mkdir -p "$dir"
  cp "$SYMBOLS/$name/functions.tsv" "$SYMBOLS/$name/jumptables.tsv" "$dir/"
  PATH="/opt/homebrew/opt/llvm/bin:$PATH" "$PYTHON" -I "$ROOT/third_party/x86recomp/tools/pe_symbols.py" \
    "$INSTALL/$file" --out "$dir" > /dev/null
  [ "$i" = 0 ] && args+=(--exe "$INSTALL/$file") || args+=(--module "$INSTALL/$file")
  args+=(--symbols "$dir" --manifest "$ROOT/config/$name.manifest")
done

rm -rf "$SCRATCH"
mkdir -p "$SCRATCH" "$OUT"
"$LIFT" "${args[@]}" --out "$SCRATCH"

# Files the new lift no longer produces go; changed ones are replaced.
for f in "$OUT"/*; do
  [ -e "$f" ] || continue
  [ -e "$SCRATCH/$(basename "$f")" ] || rm -f "$f"
done
changed=0
for f in "$SCRATCH"/*; do
  base="$(basename "$f")"
  if ! cmp -s "$f" "$OUT/$base"; then
    mv "$f" "$OUT/$base"
    changed=$((changed + 1))
  fi
done
rm -rf "$SCRATCH"
echo "lift: $changed file(s) changed"
