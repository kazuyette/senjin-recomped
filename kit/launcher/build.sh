#!/bin/sh
# NB: -D__USE_MINGW_ANSI_STDIO=0 is required (kit_launcher.c relies on MS swprintf semantics: %s = wide, %S = narrow).
# Builds the common launcher for a project (mingw cross-compilation).
# Usage: sh build.sh <project_dir> [output_dir]   (default output: <project>/release)
# The project dir holds VERSION, project.ini, launcher.ini and, optionally, icon.ico.
KIT="$(cd "$(dirname "$0")/.." && pwd)"
P="${1:?project dir}"; OUT="${2:-$P/release}"; W="$(mktemp -d)"; mkdir -p "$OUT"
python3 "$KIT/version/gen_version.py" --dir "$P" --out "$W" || exit 1
{ echo "1 24 \"$KIT/launcher/launcher.manifest\""
  [ -f "$P/icon.ico" ] && echo "2 ICON \"$P/icon.ico\""
  echo '#include "version.rc"'; } > "$W/launcher.rc"
x86_64-w64-mingw32-windres "$W/launcher.rc" -I"$W" -o "$W/res.o" || exit 1
x86_64-w64-mingw32-gcc -O2 -w -mwindows -municode -D__USE_MINGW_ANSI_STDIO=0 -I"$W" "$KIT/launcher/kit_launcher.c" "$KIT/launcher/kit_extract.c" -I"$KIT/launcher" "$W/res.o" \
  -o "$OUT/launcher.exe" -lcomctl32 -lshell32 -lole32 -lole32 -luser32 -lgdi32 && echo "LAUNCHER_OK $OUT/launcher.exe"
[ -f "$OUT/launcher.ini" ] || { [ -f "$P/launcher.ini" ] && cp "$P/launcher.ini" "$OUT/launcher.ini"; }   # the launcher reads launcher.ini next to itself; never overwritten (user settings)
cp "$W/version.h" "$P/version.h" 2>/dev/null   # the game includes the same version.h
rm -rf "$W"
