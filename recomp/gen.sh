#!/bin/sh
# Senjin Aleste: translates YOUR copy of the game executable into C (kit/recomp_x86).
# Usage: sh recomp/gen.sh /path/to/senxin_aleste_apm3.exe
# The generated C contains the game's code: it stays on your machine (recomp/gen, ignored by git). Never publish it.
P="$(cd "$(dirname "$0")/.." && pwd)"; KIT="${KIT:-$P/kit}"
EXE="${1:-$EXE}"; [ -f "$EXE" ] || { echo "usage: sh recomp/gen.sh /path/to/senxin_aleste_apm3.exe"; exit 1; }
want="$(cut -d' ' -f1 "$P/recomp/exe.sha256")"; got="$(sha256sum "$EXE" | cut -d' ' -f1)"
if [ "$want" != "$got" ] && [ -z "$FORCE" ]; then
  echo "This executable is not the supported version (Senjin Aleste APM3, VER 1.02)."
  echo "  expected: $want"; echo "  found:    $got"; echo "Set FORCE=1 to try anyway (unsupported)."; exit 1
fi
mkdir -p "$P/recomp/gen"; rm -f "$P"/recomp/gen/rc_*.c
python3 "$KIT/recomp_x86/x86recomp.py" "$EXE" "$P/recomp/recomp_info.json" --out "$P/recomp/gen" && cp "$KIT/recomp_x86/x86rt.h" "$P/recomp/gen/"
