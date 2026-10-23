#!/bin/bash
# Translate the shader variants an iPhone/iPad missed and push them back to it.
#
# The device records a request under Documents/shader-requests/ for every variant it lacked; this script pulls them,
# translates them on this Mac (totk_xlate + SPIRV-Cross), compiles iOS metallibs, and pushes one pack to
# Documents/shader-fill/. The game merges and warms the pack on its next start.
#
# Environment:
#   TOTK_TREE       prepared work tree (build/spirv-cross, prebuilt/macos-arm64/totk_xlate)
#   TOTK_IOS_DEVICE devicectl UDID of the device
#   TOTK_BUNDLE_ID  bundle id of the installed app
# Extra arguments go to fill_shaders.py (--no-push, --with-done, --requests ...).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
TOOL="$REPO/games/totk/ios/tools/fill_shaders.py"
TREE="${TOTK_TREE:?set TOTK_TREE to the prepared work tree}"

# --requests <dir> with --no-push rebuilds a saved pull on this Mac: no device is read from or written to, so the
# wrapper must not demand one either (fill_shaders.py accepts exactly this combination without --device).
OFFLINE=0; HAS_REQUESTS=0; NO_PUSH=0
for arg in "$@"; do
    case "$arg" in
        --requests|--requests=*) HAS_REQUESTS=1 ;;
        --no-push) NO_PUSH=1 ;;
    esac
done
if [ "$HAS_REQUESTS" = 1 ] && [ "$NO_PUSH" = 1 ]; then OFFLINE=1; fi
if (( OFFLINE )); then
    DEVICE=""; BUNDLE=""
else
    DEVICE="${TOTK_IOS_DEVICE:?set TOTK_IOS_DEVICE to the device UDID}"
    BUNDLE="${TOTK_BUNDLE_ID:?set TOTK_BUNDLE_ID to the app bundle id}"
fi

step() { printf '\n==> %s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

step "1/4 检查环境"
[ -x "$TREE/build/spirv-cross/spirv-cross" ] || fail "缺少 $TREE/build/spirv-cross/spirv-cross；先构建一次"
[ -f "$TOOL" ] || fail "缺少 $TOOL"
command -v xcrun >/dev/null || fail "找不到 xcrun；请安装完整 Xcode"
command -v python3 >/dev/null || fail "找不到 python3"
printf '  仓库:   %s\n  工作树: %s\n' "$REPO" "$TREE"

if (( OFFLINE )); then
    step "2/4 跳过设备检查（--requests 配 --no-push，只用本地请求）"
else
    step "2/4 检查 iPad"
    if ! xcrun devicectl list devices 2>/dev/null | awk -v id="$DEVICE" 'index($0, id) && $0 !~ /unavailable/ { found = 1 } END { exit !found }'; then
        fail "iPad 未连接或不可用：$DEVICE"
    fi
    APPS=""
    for attempt in 1 2 3; do
        if APPS="$(xcrun devicectl device info apps --device "$DEVICE" 2>&1)"; then break; fi
        [ "$attempt" -eq 3 ] && fail "无法读取 iPad 的 App 列表；请解锁 iPad、保持连接后重试"
        sleep 1
    done
    [ -n "$APPS" ] || fail "iPad 返回了空的 App 列表；请解锁 iPad 后重试"
    if ! grep -Fq -- "$BUNDLE" <<< "$APPS"; then   # here-string: a pipe would let grep -q exit early and SIGPIPE the producer (pipefail)
        fail "iPad 上没有安装 $BUNDLE"
    fi
    printf '  设备在线，App 已安装\n  如果接下来的读取失败，请解锁 iPad 后重试\n'
fi

step "3/4 拉取请求、翻译、编译并推送"
LOG="$(mktemp -t totk-fill)"
trap 'rm -f "$LOG"' EXIT
CMD=(python3 "$TOOL" --tree "$TREE")
(( OFFLINE )) || CMD+=(--device "$DEVICE" --bundle-id "$BUNDLE")
if ! "${CMD[@]}" "$@" 2>&1 | tee "$LOG"; then
    if (( OFFLINE )); then
        printf '\n补译没有完成。检查 --requests 指向的目录里是否有请求文件。\n' >&2
    else
        printf '\n补译没有完成。请确认 iPad 已解锁并保持连接，然后重试。\n' >&2
    fi
    exit 1
fi

step "4/4 完成"
if grep -q "no shader requests" "$LOG"; then
    printf '  iPad 上还没有新的缺失着色器请求；继续玩即可，以后再运行这个脚本。\n'
elif grep -q "^not pushed" "$LOG"; then
    printf '  补译包已生成，但没有推送到 iPad（--no-push）：%s\n' "$(sed -n 's/^not pushed; the pack is in //p' "$LOG")"
else
    printf '  请在 iPad 上完全退出游戏，再重新打开；启动时会合并并预热新的着色器。\n'
fi
