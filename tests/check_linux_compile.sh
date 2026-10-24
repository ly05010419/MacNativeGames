#!/usr/bin/env bash
# check_linux_compile.sh — how much of the platform-neutral C layer compiles on a
# host that has no Apple SDK?
#
# Runs `clang -fsyntax-only` over the C sources of the runtime and the graphics
# layer, then classifies every failure so the number is actionable instead of
# alarming:
#
#   PASS   compiles and its code is active
#   PASS*  compiles, but its `#if TARGET_OS_*` branch is disabled by the shim:
#          syntax-clean, not functional — do not count it as portability
#   PORT   needs a real Apple API (AudioToolbox, mach, CoreGraphics, ...)
#   ARM    needs ARM intrinsics: fine on Linux/arm64, not on x86_64
#   DEP    a library or generated header is missing (build input, not a port task)
#
# Usage:
#   tests/check_linux_compile.sh                 # report, always exit 0
#   tests/check_linux_compile.sh --verbose       # per-file detail
#   tests/check_linux_compile.sh --min-pass 15   # ratchet: exit 1 below 15 PASS
#
# Needs: clang (or $CC), bash. No Apple SDK, no Vulkan headers, no game data.

set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

CC="${CC:-clang}"
VERBOSE=""
MIN_PASS=0

while [ $# -gt 0 ]; do
  case "$1" in
    --verbose) VERBOSE=1 ;;
    --min-pass) MIN_PASS="${2:-0}"; shift ;;
    -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
  shift
done

command -v "$CC" >/dev/null 2>&1 || { echo "error: '$CC' not found (set CC=...)" >&2; exit 2; }

# Project include paths, plus the Apple-header shim so the `#if TARGET_OS_*`
# files resolve on a non-Apple host.
INC=(-Icompat/linux -I. -Igames/totk -Igames/totk/runtime -Igames/totk/third_party
     -Igames/totk/third_party/d2r_gfx
     -Igames/totk/third_party/d2r_gfx/gfx
     -Igames/totk/third_party/d2r_gfx/gfx/nvn
     -Igames/totk/third_party/d2r_gfx/gfx/nvn/include
     -Igames/totk/third_party/d2r_gfx/gfx/tex)
FLAGS=(-D_GNU_SOURCE -std=gnu11 -fsyntax-only)

# Headers whose absence is a real port task rather than a missing dependency.
APPLE_H='TargetConditionals|AudioToolbox|AudioUnit|CoreFoundation|CoreGraphics|CoreText|Cocoa|AppKit|Foundation|QuartzCore|CoreVideo|ImageIO|Vision|Metal|libproc|libkern|CommonCrypto|mach/|mach-o/|malloc/malloc|dispatch/|os/|objc/'
# Darwin functions/constants reached without an Apple header (via another include).
APPLE_FN='pthread_threadid_np|clock_gettime_nsec_np|CLOCK_UPTIME_RAW|F_FULLFSYNC|arc4random_buf|mach_vm_|vm_remap|vm_allocate|sysctlbyname|OSAtomic|kern_return|mach_absolute_time|CACurrentMediaTime|os_unfair_lock'
# ARM intrinsics: valid on Linux/arm64, fail only on x86_64.
ARM_H='arm_neon|vld1q_|vst2q_|vst1q_|uint8x[0-9]+x?[0-9]*_t|vgetq_lane'

DIRS=(games/totk/runtime
      games/totk/third_party/d2r_gfx/gfx
      games/totk/third_party/d2r_gfx/gfx/nvn
      games/totk/third_party/d2r_gfx/gfx/tex)

pass_n=0;   pass_loc=0
hollow_n=0; hollow_loc=0
port_n=0;   port_loc=0
arm_n=0;    arm_loc=0
dep_n=0;    dep_loc=0
other_n=0;  other_loc=0

for d in "${DIRS[@]}"; do
  for f in "$d"/*.c; do
    [ -e "$f" ] || continue
    err="$("$CC" "${FLAGS[@]}" "${INC[@]}" "$f" 2>&1)"
    loc=$(wc -l < "$f")
    if [ -z "$err" ]; then
      if grep -q 'TARGET_OS_' "$f"; then
        # Compiles, but its platform branch is off: syntax-clean, not functional.
        hollow_n=$((hollow_n+1)); hollow_loc=$((hollow_loc+loc))
        [ -n "$VERBOSE" ] && printf '  PASS* %-58s %5d LOC  (TARGET_OS_* branch off)\n' "$f" "$loc"
      else
        pass_n=$((pass_n+1)); pass_loc=$((pass_loc+loc))
        [ -n "$VERBOSE" ] && printf '  PASS  %-62s %5d LOC\n' "$f" "$loc"
      fi
      continue
    fi
    if echo "$err" | grep -qE "#include <($APPLE_H)[^>]*>|$APPLE_FN"; then
      port_n=$((port_n+1)); port_loc=$((port_loc+loc)); cls="PORT"
    elif echo "$err" | grep -qE "$ARM_H"; then
      arm_n=$((arm_n+1)); arm_loc=$((arm_loc+loc)); cls="ARM "
    elif echo "$err" | grep -q "file not found"; then
      dep_n=$((dep_n+1)); dep_loc=$((dep_loc+loc)); cls="DEP "
    else
      other_n=$((other_n+1)); other_loc=$((other_loc+loc)); cls="OTRO"
    fi
    [ -n "$VERBOSE" ] && {
      printf '  %s %-59s %5d LOC\n' "$cls" "$f" "$loc"
      echo "$err" | grep -E 'error:' | head -3 | sed 's/^/        /'
    }
  done
done

total_n=$((pass_n+hollow_n+dep_n+port_n+arm_n+other_n))
total_loc=$((pass_loc+hollow_loc+dep_loc+port_loc+arm_loc+other_loc))
pct() { [ "$total_loc" -gt 0 ] && echo "$(( $1*100/total_loc ))%" || echo "-"; }

row() { printf ' %-40s %8s %10s %8s\n' "$1" "$2" "$3" "$4"; }

echo
echo "==================================================================="
echo " Linux syntax check — $CC -fsyntax-only (no link, no execution)"
echo "==================================================================="
row "CLASS" "files" "LOC" "% LOC"
echo " ------------------------------------------------------------------"
row "PASS   compiles, code active"            "$pass_n"  "$pass_loc"  "$(pct "$pass_loc")"
row "PASS*  compiles, TARGET_OS_* branch off" "$hollow_n" "$hollow_loc" "-"
row "PORT   Apple API  <- real port work"     "$port_n"  "$port_loc"  "$(pct "$port_loc")"
row "ARM    NEON intrinsics (ok on arm64)"    "$arm_n"   "$arm_loc"   "-"
row "DEP    missing library / include"        "$dep_n"   "$dep_loc"   "-"
row "OTRO   other error"                      "$other_n" "$other_loc" "-"
echo " ------------------------------------------------------------------"
row "TOTAL" "$total_n" "$total_loc" ""
echo "==================================================================="
echo "PASS* = syntax-clean only: the shim turns TARGET_OS_* off, so that"
echo "        platform branch is not being compiled."
echo "PORT  = the actionable number. It counts .c only; the .m/Metal layer"
echo "        is a separate, larger task."
echo

if [ -n "$VERBOSE" ]; then :; else echo "(run with --verbose for per-file detail)"; echo; fi

if [ "$MIN_PASS" -gt 0 ] && [ "$pass_n" -lt "$MIN_PASS" ]; then
  echo "REGRESSION: $pass_n files compile, expected at least $MIN_PASS." >&2
  exit 1
fi
exit 0
