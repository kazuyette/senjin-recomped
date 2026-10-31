#!/bin/sh
# Senjin Aleste - native Linux port: one-step build.
# Usage: ./build.sh /path/to/senxin_aleste_apm3.exe
#   1. translates your executable into C (recomp/gen)      - needs python3 + capstone + pefile
#   2. compiles it with the native layer into out/senjin_aleste - needs gcc, SDL2, OpenGL, FreeType, fontconfig
# Windows tools (apm_x86.dll, launcher.exe, aleste_pad.exe): sh build_windows.sh (mingw-w64).
P="$(cd "$(dirname "$0")" && pwd)"
[ -n "$1" ] || { echo "usage: ./build.sh /path/to/senxin_aleste_apm3.exe"; exit 1; }
python3 -c "import capstone, pefile" 2>/dev/null || { echo "missing Python modules: pip install capstone pefile"; exit 1; }
for t in gcc sdl2-config pkg-config; do command -v $t >/dev/null || { echo "missing: $t (Debian/Ubuntu: sudo apt install build-essential libsdl2-dev libfreetype-dev libfontconfig-dev libgl-dev pkg-config)"; exit 1; }; done
sh "$P/recomp/gen.sh" "$1" || exit 1
sh "$P/build_native.sh" || exit 1
echo "Done: out/senjin_aleste - run it with ./play.sh /path/to/game/folder"
