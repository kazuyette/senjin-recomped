#!/bin/sh
# Senjin Aleste: builds the native port (Linux 64-bit) from the generated C (recomp/gen). Usage: sh build_native.sh [output_dir]
# (normally called by ./build.sh, which first generates the C from your exe)
# The generated C (recomp/gen) is compiled once (cached .o files); the native layer is rebuilt every time.
P="$(cd "$(dirname "$0")" && pwd)"; KIT="${KIT:-$P/kit}"; OUT="${1:-$P/out}"; B="$P/recomp/build"
mkdir -p "$B" "$OUT"
python3 "$KIT/version/gen_version.py" --dir "$P" --out "$B" >/dev/null || exit 1
CF="-O1 -w -fno-strict-aliasing $(sdl2-config --cflags) $(pkg-config --cflags freetype2 fontconfig) -I$P/recomp/gen -I$KIT/recomp_x86 -I$P/native -I$B"
for c in "$P"/recomp/gen/rc_*.c; do o="$B/$(basename "$c" .c).o"; [ "$o" -nt "$c" ] || echo "$c"; done | xargs -r -P 2 -I{} sh -c 'gcc '"$CF"' -c {} -o '"$B"'/$(basename {} .c).o' || exit 1
gcc $CF -g -c "$KIT/recomp_x86/x86rt.c" -o "$B/x86rt.o" || exit 1
for c in "$P"/native/*.c; do gcc $CF -g -c "$c" -o "$B/n_$(basename "$c" .c).o" || exit 1; done
for o in "$B"/rc_*.o; do [ -f "$P/recomp/gen/$(basename "$o" .o).c" ] || rm -f "$o"; done
gcc -o "$OUT/senjin_aleste" "$B"/rc_*.o "$B/x86rt.o" "$B"/n_*.o -lm -lpthread $(sdl2-config --libs) $(pkg-config --libs freetype2 fontconfig) -lGL && echo "NATIF_OK $OUT/senjin_aleste"
