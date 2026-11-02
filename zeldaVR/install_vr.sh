#!/bin/bash
# Zelda VR: Tears of the Kingdom 1.4.3 on a Meta Quest 3 over the cable, in one step (zeldaVR/install.md explains it).
# Each step is skipped when it is done and its inputs did not change, so running it again redoes only what changed.
#
#   zeldaVR/install_vr.sh -- "<base 1.0.0 xci/nsp>" -k "<prod.keys>" --update "<1.4.3 update>"
#
# Steps: 1 the game app with VR (build_app.py --vr, the arguments after --); 2 the streamer (BasaltVR at BASALT_REV +
# zeldaVR/streamer/basaltvr.patch, swift build); 3 the Quest on the cable: ALVR ALVR_VERSION (SHA-256 checked), its
# microphone question answered for good; 4 "塞尔达 VR" on the Desktop: wakes the Quest, starts ALVR, opens the game.
# Options: --work <folder> (default ~/zeldaVR-work: the app, the streamer, the build folder), --no-app, --skip-quest,
# --no-desktop, --name <Desktop name>.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
BASALT_URL=https://github.com/frenchbaguetteman/BasaltVR
BASALT_REV=4c46172
ALVR_VERSION=v20.14.0
ALVR_URL=https://github.com/alvr-org/ALVR/releases/download/$ALVR_VERSION/alvr_client_android.apk
ALVR_SHA256=61979e9b153f237e4f06d838b506b677255bb6561ff3eaa694ad825f7367f91f
ALVR_PKG=alvr.client.stable

WORK=$HOME/zeldaVR-work; APPSTEP=1; QUEST=1; DESKTOP=1; NAME="塞尔达 VR"; NARGS=0
while [ $# -gt 0 ]; do
  case $1 in
    --work) WORK=$2; shift 2;;
    --no-app) APPSTEP=0; shift;;
    --skip-quest) QUEST=0; shift;;
    --no-desktop) DESKTOP=0; shift;;
    --name) NAME=$2; shift 2;;
    --) shift; NARGS=$#; break;;   # "$@" is now build_app.py's arguments (macOS bash 3.2: no empty arrays under set -u)
    *) echo "unknown option $1 (the game files go after --)" >&2; exit 2;;
  esac
done
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
APP="$WORK/app/Tears of the Kingdom.app"; BASALT=$WORK/BasaltVR
STREAMER=$BASALT/streamer/.build/release/basalt-streamer
say() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
sha() { shasum -a 256 "$@" | cut -d' ' -f1; }

# 1. The game app with VR's picture modes (build_app.py decides itself what to rebuild).
if [ $APPSTEP = 0 ]; then
  say "1/4 app: skipped (--no-app)"
elif [ $NARGS -eq 0 ]; then
  [ -d "$APP" ] || die "no app yet: pass the game files after --, as for build_app.py"
  say "1/4 app: kept ($APP; pass the game files after -- to build it again)"
else
  pgrep -q '^totk-native' && die "a game is running: close it first (packaging uses the GPU)"
  PY=$(command -v python3.14 || command -v python3)
  say "1/4 app: build_app.py --vr -> $APP"
  (cd "$REPO" && "$PY" build_app.py "$@" --vr -o "$APP" --work "$WORK/build-work")
fi

# 2. The streamer: BasaltVR at its revision with our patch, built again when the patch changed.
PATCH=$HERE/streamer/basaltvr.patch
WANT=$( { echo "$BASALT_REV"; sha "$PATCH"; } | sha)
if [ -x "$STREAMER" ] && [ "$(cat "$BASALT/.zeldavr" 2>/dev/null)" = "$WANT" ]; then
  say "2/4 streamer: up to date"
else
  command -v swift >/dev/null || die "swift is missing: install Xcode or its command line tools (xcode-select --install)"
  say "2/4 streamer: BasaltVR $BASALT_REV + basaltvr.patch"
  [ -d "$BASALT/.git" ] || git clone -q "$BASALT_URL" "$BASALT"
  git -C "$BASALT" fetch -q origin 2>/dev/null || true
  git -C "$BASALT" checkout -q --force "$BASALT_REV"; git -C "$BASALT" clean -qfd -e .build
  git -C "$BASALT" apply "$PATCH" || die "basaltvr.patch does not apply on BasaltVR $BASALT_REV"
  (cd "$BASALT/streamer" && swift build -c release --product basalt-streamer) || die "swift build failed"
  echo "$WANT" > "$BASALT/.zeldavr"
fi

# 3. The Quest: ALVR's client, checked by its SHA-256; its microphone question denied for good (it hid the handshake).
ADB=$(command -v adb || echo /opt/homebrew/bin/adb)
if [ $QUEST = 0 ]; then
  say "3/4 Quest: skipped (--skip-quest)"
else
  if [ ! -x "$ADB" ]; then
    command -v brew >/dev/null || die "adb is missing: install Homebrew (brew.sh), then run this again"
    brew install android-platform-tools; ADB=$(command -v adb)
  fi
  case $("$ADB" get-state 2>&1) in
    device) ;;
    *unauthorized*) die "the Quest asks to allow USB debugging: put it on, tick Always allow, then run this again";;
    *) die "no Quest on the cable (developer mode and USB debugging, zeldaVR/install.md); --skip-quest installs the rest";;
  esac
  if "$ADB" shell pm list packages "$ALVR_PKG" | grep -q "$ALVR_PKG"; then
    say "3/4 Quest: ALVR is installed"
  else
    APK=$WORK/downloads/alvr-$ALVR_VERSION.apk; mkdir -p "$WORK/downloads"
    if [ ! -f "$APK" ] || [ "$(sha "$APK")" != "$ALVR_SHA256" ]; then say "3/4 Quest: downloading ALVR $ALVR_VERSION"; curl -fL -o "$APK" "$ALVR_URL"; fi
    [ "$(sha "$APK")" = "$ALVR_SHA256" ] || die "ALVR download: SHA-256 $(sha "$APK"), want $ALVR_SHA256"
    say "3/4 Quest: installing ALVR $ALVR_VERSION"; "$ADB" install -r "$APK"
  fi
  "$ADB" shell pm set-permission-flags "$ALVR_PKG" android.permission.RECORD_AUDIO user-set user-fixed >/dev/null 2>&1 || true
fi

# 4. The launcher: a script beside the app, and a Desktop applet that runs it.
LAUNCH=$WORK/start_vr.sh
cat > "$LAUNCH" <<EOF
#!/bin/zsh
# Made by zeldaVR/install_vr.sh: wakes the Quest, starts ALVR on it, opens the game with VR (it starts the streamer itself).
alert() { osascript -e "display alert \"$NAME\" message \"\$1\"" >/dev/null 2>&1; }
pgrep -q '^totk-native' && { alert "已经有一个游戏在运行，先关掉它。A game is already running: close it first."; exit 1; }
ADB=\$(command -v adb || echo /opt/homebrew/bin/adb)
if "\$ADB" get-state >/dev/null 2>&1; then
  "\$ADB" shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null 2>&1   # stays on while off the face
  "\$ADB" shell monkey -p $ALVR_PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
else
  alert "没有连上 Quest（数据线和 USB 调试）。游戏照常启动，连上后在 Quest 里打开 ALVR。No Quest on the cable: the game starts anyway; open ALVR on the Quest once it is connected."
fi
open -n --env TOTK_VR_CONTROL="$WORK/control.txt" --env TOTK_VR_STREAMER="$STREAMER" --env TOTK_VR_MBPS=150 "$APP"
EOF
chmod +x "$LAUNCH"
if [ $DESKTOP = 1 ]; then
  osacompile -o "$HOME/Desktop/$NAME.app" -e "do shell script quoted form of \"$LAUNCH\" & \" > /dev/null 2>&1 &\""
  say "4/4 Desktop: \"$NAME\" starts VR"
else
  say "4/4 launcher: $LAUNCH (no Desktop icon: --no-desktop)"
fi
say "done: put the Quest on, then double-click \"$NAME\" (or run $LAUNCH)"
