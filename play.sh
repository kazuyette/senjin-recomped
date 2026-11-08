#!/bin/bash
# Senjin Aleste - native Linux port: run the game (Linux or WSL2/WSLg).
# Usage: ./play.sh /path/to/game/folder [options]      (the folder that contains senxin_aleste_apm3.exe and windata/)
#   ./play.sh deps                  install runtime dependencies (Debian/Ubuntu, sudo)
#   ./play.sh info                  show the OpenGL renderer that will be used
#   ./play.sh sound                 play a 2 s test tone
#   ./play.sh <dir> --soft          software rendering (llvmpipe)
#   ./play.sh <dir> --capture=N     save a screenshot after N seconds (capture.bmp in the game folder)
#   ./play.sh <dir> -v              verbose log
#   ./play.sh deck <dir>            Steam Deck package: out/senjin-aleste-deck.tar (see STEAMDECK.md)
# Keys: arrows, Enter = Start, Z X C V = Shot / Power-up / Rapid / Blow, P = Pause, F8 = bezel on/off, F9 = CRT filter, Esc = quit.
# Settings: aleste.ini in the game folder (written by launcher.exe on Windows, or edit it by hand).
P="$(cd "$(dirname "$0")" && pwd)"
BIN="${SA_BIN:-$P/out/senjin_aleste}"; PAD="$P/out/windows/aleste_pad.exe"
WSL=0; grep -qi microsoft /proc/version 2>/dev/null && WSL=1
deps() { sudo apt-get update && sudo apt-get install -y libsdl2-2.0-0 ffmpeg mesa-utils libgl1-mesa-dri rsync libfreetype6 libfontconfig1 fonts-noto-cjk; }
gpu_env() { if [ $WSL = 1 ]; then export GALLIUM_DRIVER="${GALLIUM_DRIVER:-d3d12}"; fi; }   # WSL2: OpenGL -> Direct3D 12 -> Windows GPU
info() { gpu_env; glxinfo -B 2>/dev/null | grep -E "OpenGL renderer|OpenGL core profile version|Accelerated" || echo "glxinfo not found: ./play.sh deps"; }
# Steam Deck package: game binary + static ffmpeg + Japanese font + the user's game files + Deck settings, in one .tar
deck() {
  local G; G="$(cd "$1" 2>/dev/null && pwd)"
  [ -f "$G/senxin_aleste_apm3.exe" ] && [ -d "$G/windata" ] || { echo "usage: ./play.sh deck /path/to/game/folder (senxin_aleste_apm3.exe + windata/)"; return 1; }
  [ -x "$BIN" ] || { echo "out/senjin_aleste not found: build it first with ./build.sh /path/to/senxin_aleste_apm3.exe"; return 1; }
  local O="$P/out/deck" D="$P/out/deck/senjin-aleste-deck"; rm -rf "$D"; mkdir -p "$D/bin" "$D/fonts" "$D/game" || return 1
  echo "== game binary"; cp "$BIN" "$D/bin/" || return 1
  echo "== ffmpeg (static build, decodes the game's xWMA sound)"
  local FF="${FFMPEG_STATIC:-$O/ffmpeg-static.tar.xz}"
  if [ ! -s "$FF" ]; then echo "downloading (40 MB)..."; curl -fsSL -o "$FF" https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz || { echo "download failed: put a static ffmpeg tar.xz in $FF (or set FFMPEG_STATIC)"; return 1; }; fi
  tar -xJf "$FF" -C "$O" --wildcards '*/ffmpeg' && mv -f "$O"/ffmpeg-*-static/ffmpeg "$D/bin/ffmpeg" && rm -rf "$O"/ffmpeg-*-static || { echo "bad ffmpeg archive: $FF"; return 1; }
  echo "== Japanese font"
  local F; F="$(fc-match -f '%{file}' 'Noto Sans CJK JP:lang=ja' 2>/dev/null)"; [ -f "$F" ] || F="$(fc-match -f '%{file}' ':lang=ja' 2>/dev/null)"
  [ -f "$F" ] || { echo "no Japanese font found: ./play.sh deps"; return 1; }
  cp "$F" "$D/fonts/" || return 1
  echo "== game files (copied from $G)"
  tar -C "$G" -chf - --exclude='*.bmp' --exclude='capture.req' --exclude='*.log' --exclude='fd_*' . | tar -C "$D/game" -xf - || return 1
  python3 - "$D/game/aleste.ini" <<'PY'
import sys, os, re
p = sys.argv[1]; s = open(p, encoding='latin-1').read() if os.path.exists(p) else ''
def put(sec, key, val):
    global s
    m = re.search(r'(?m)^\[' + re.escape(sec) + r'\]\r?\n', s)
    if not m: s += ('' if s.endswith('\n') or not s else '\r\n') + '[%s]\r\n%s=%s\r\n' % (sec, key, val); return
    body_end = re.search(r'(?m)^\[', s[m.end():]); end = m.end() + (body_end.start() if body_end else len(s) - m.end())
    body = s[m.end():end]; nb, n = re.subn(r'(?m)^' + re.escape(key) + r'=.*$', '%s=%s' % (key, val), body)
    if not n: nb = '%s=%s\r\n' % (key, val) + body
    s = s[:m.end()] + nb + s[end:]
put('config', 'FULLSCREEN', '1'); put('video', 'latency', '1')
open(p, 'w', encoding='latin-1').write(s)
c = os.path.join(os.path.dirname(p), 'config.txt')                  # the game's own file: fullscreen there too
if os.path.exists(c):
    t = open(c, encoding='latin-1').read(); t2, n = re.subn(r'(?m)^FULLSCREEN=\d+', 'FULLSCREEN=1', t)
    if n: open(c, 'w', encoding='latin-1').write(t2)
PY
  cat > "$D/aleste-deck.sh" <<'SH'
#!/bin/bash
# Senjin Aleste - Steam Deck launcher. Add this file to Steam as a non-Steam game (see README-SteamDeck.txt).
D="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"; LOG="$D/deck.log"
chmod +x "$D/bin/"* 2>/dev/null
export PATH="$D/bin:$PATH"
for f in "$D"/fonts/*; do [ -f "$f" ] && export SA_FONT="$f"; done
MISS="$(ldd "$D/bin/senjin_aleste" 2>&1 | grep 'not found')"
if [ -n "$MISS" ]; then { echo "Senjin Aleste cannot start: missing system libraries"; echo "$MISS"; } > "$LOG"; exit 1; fi
cd "$D/game" && exec "$D/bin/senjin_aleste" . > "$LOG" 2>&1
SH
  chmod +x "$D/aleste-deck.sh" "$D/bin/"*
  cp "$P/STEAMDECK.md" "$D/README-SteamDeck.txt" 2>/dev/null
  echo "== archive"; tar -cf "$O/senjin-aleste-deck.tar" -C "$O" senjin-aleste-deck || return 1
  echo "done: $O/senjin-aleste-deck.tar ($(du -h "$O/senjin-aleste-deck.tar" | cut -f1)). Copy it to the Steam Deck and follow README-SteamDeck.txt."
}
case "$1" in
  deps) deps; exit $? ;;
  info) info; exit 0 ;;
  sound) ffmpeg -v error -f lavfi -i "sine=frequency=440:duration=2" -f pulse "aleste" && echo "tone sent"; exit 0 ;;
  deck) shift; deck "$@"; exit $? ;;
  ""|-h|--help) sed -n 2,12p "$0"; exit 0 ;;
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
[ "$RUN" != "$GAME" ] && cp -f "$RUN/aleste.ini" "$GAME/aleste.ini" 2>/dev/null   # settings changed in the game, back to the original folder
[ -n "$PADPID" ] && kill $PADPID 2>/dev/null
exit 0
