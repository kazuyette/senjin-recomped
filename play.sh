#!/bin/bash
# Senjin Aleste - native Linux port: run the game (Linux or WSL2/WSLg).
# Usage: ./play.sh /path/to/game/folder [options]      (the folder that contains senxin_aleste_apm3.exe and windata/)
#   ./play.sh deps                  install runtime dependencies (Debian/Ubuntu, sudo)
#   ./play.sh info                  show the OpenGL renderer that will be used
#   ./play.sh sound                 play a 2 s test tone
#   ./play.sh <dir> --soft          software rendering (llvmpipe)
#   ./play.sh <dir> --capture=N     save a screenshot after N seconds (capture.bmp in the game folder)
#   ./play.sh <dir> -v              verbose log
# Keys: arrows, Enter = Start, Z X C V = Shot / Power-up / Rapid / Blow, P = Pause, F8 = bezel on/off, F9 = CRT filter, Esc = quit.
# Settings: aleste.ini in the game folder (written by launcher.exe on Windows, or edit it by hand).
P="$(cd "$(dirname "$0")" && pwd)"
BIN="$P/out/senjin_aleste"; PAD="$P/out/windows/aleste_pad.exe"
WSL=0; grep -qi microsoft /proc/version 2>/dev/null && WSL=1
deps() { sudo apt-get update && sudo apt-get install -y libsdl2-2.0-0 ffmpeg mesa-utils libgl1-mesa-dri rsync libfreetype6 libfontconfig1 fonts-noto-cjk; }
gpu_env() { if [ $WSL = 1 ]; then export GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}"; fi; }   # WSL2: OpenGL -> Direct3D 12 -> Windows GPU
info() { gpu_env; glxinfo -B 2>/dev/null | grep -E "OpenGL renderer|OpenGL core profile version|Accelerated" || echo "glxinfo not found: ./play.sh deps"; }
case "$1" in
  deps) deps; exit $? ;;
  info) info; exit 0 ;;
  sound) ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=2" -f pulse "aleste" && echo "tone sent"; exit 0 ;;
  ""|-h|--help) sed -n 2,11p "$0"; exit 0 ;;
esac
GAME="$(cd "$1" 2>/dev/null && pwd)"; shift
[ -f "$GAME/senxin_aleste_apm3.exe" ] && [ -d "$GAME/windata" ] || { echo "not a game folder (senxin_aleste_apm3.exe + windata/ expected): $GAME"; exit 1; }
[ -x "$BIN" ] || { echo "out/senjin_aleste not found: build it first with ./build.sh /path/to/senxin_aleste_apm3.exe"; exit 1; }
command -v ffmpeg >/dev/null && fc-list :lang=ja 2>/dev/null | grep -q . || { echo "missing runtime dependencies (ffmpeg, a Japanese font): ./play.sh deps"; exit 1; }
ARGS=(); CAP=0
for a in "$@"; do case "$a" in --soft) export LIBGL_ALWAYS_SOFTWARE=1 ;; --capture=*) CAP="${a#--capture=}" ;; *) ARGS+=("$a") ;; esac; done
RUN="$GAME"
if [ $WSL = 1 ] && case "$GAME" in /mnt/*) true ;; *) false ;; esac; then
  # Windows drives are slow from WSL2: play from a copy on the Linux disk (saves are kept there; settings come from the original)
  RUN="${ALESTE_DIR:-$HOME/.local/share/senjin-aleste/game}"; mkdir -p "$RUN"
  echo "== copying the game to $RUN"
  rsync -a --update --exclude '*.bmp' --exclude 'capture.req' "$GAME/" "$RUN/" || exit 1
  cp -f "$GAME/aleste.ini" "$RUN/aleste.ini" 2>/dev/null
fi
[ -n "$LIBGL_ALWAYS_SOFTWARE" ] || gpu_env
rm -f "$RUN/capture.req"
[ "$CAP" -gt 0 ] 2>/dev/null && ( sleep "$CAP"; touch "$RUN/capture.req" ) &
PADPID=
if [ $WSL = 1 ] && [ -f "$PAD" ]; then   # WSL2 cannot see USB gamepads: Windows-side XInput relay over localhost TCP
  export SA_PADNET="${SA_PADNET:-47011}"; ( cd "$(dirname "$PAD")" && ./aleste_pad.exe "$SA_PADNET" ) & PADPID=$!
fi
cd "$RUN" && "$BIN" . "${ARGS[@]}" 2>&1 | tee "$RUN/senjin_aleste.log"
[ -n "$PADPID" ] && kill $PADPID 2>/dev/null
exit 0
