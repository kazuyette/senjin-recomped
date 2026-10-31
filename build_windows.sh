#!/bin/sh
# Builds the Windows tools (mingw-w64): apm_x86.dll (native APM3 services), aleste_pad.exe (WSL2 gamepad relay)
# and launcher.exe. Usage: sh build_windows.sh [output_dir]  (default: out/windows)
P="$(cd "$(dirname "$0")" && pwd)"; KIT="${KIT:-$P/kit}"; OUT="${1:-$P/out/windows}"; W="$(mktemp -d)"; mkdir -p "$OUT"
python3 "$KIT/version/gen_version.py" --dir "$P" --out "$W" || exit 1
sed -e "s/\"OriginalFilename\", \"[^\"]*\"/\"OriginalFilename\", \"apm_x86.dll\"/" -e "s/\"FileDescription\", \"[^\"]*\"/\"FileDescription\", \"Senjin Aleste - native APM3 services\"/" "$W/version.rc" > "$W/apm.rc"
i686-w64-mingw32-windres "$W/apm.rc" -I"$W" -o "$W/res.o" || exit 1
i686-w64-mingw32-gcc -O2 -Wall -Wno-unused-parameter -shared -s -static-libgcc -I"$W" "$P/src/apm3/apm3.c" "$P/src/apm3/video.c" "$P/src/apm3/d3dstats.c" "$P/src/apm3/d3dcache.c" "$P/src/apm3/d3dnobezel.c" "$P/src/apm3/pause.c" "$KIT/crt/kit_crt.c" -I"$KIT/crt" "$P/src/apm3/apm_x86.def" "$W/res.o" \
  -o "$OUT/apm_x86.dll" -Wl,--enable-stdcall-fixup && echo "APM_OK $OUT/apm_x86.dll"
i686-w64-mingw32-gcc -O2 -s -I"$W" -o "$OUT/aleste_pad.exe" "$P/src/pad/aleste_pad.c" -lws2_32 && echo "PAD_OK $OUT/aleste_pad.exe"
rm -rf "$W"
sh "$KIT/launcher/build.sh" "$P" "$OUT"
