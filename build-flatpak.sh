#!/bin/sh
# Build and install the Flatpak locally.
#
# Run this inside `toolbox enter` (the Fedora 45 toolbox has flatpak-builder),
# from the repository root:
#
#   ./build-flatpak.sh "/path/to/your/game/dump.zip"
#
# or copy your dump to game.zip and run:
#
#   ./build-flatpak.sh
#
# The manifest takes a generic game.zip: the repository never hardcodes the
# name of a particular dump.  game.zip and the source snapshot are ignored by
# git; only your own copy is used.
set -eu
cd "$(dirname "$0")"

# 1. Source snapshot (the release zip, not the game).
if [ ! -f senjin-recomped-0.6.21.zip ]; then
    for p in ../senjin-recomped-0.6.21.zip ../../senjin-recomped-0.6.21.zip; do
        if [ -f "$p" ]; then
            cp -f "$p" .
            break
        fi
    done
fi
[ -f senjin-recomped-0.6.21.zip ] || {
    echo "error: senjin-recomped-0.6.21.zip not found in $(pwd)" >&2
    exit 1
}

# 2. The user's game data, under the generic name the manifest expects.
if [ $# -ge 1 ]; then
    if [ ! -e game.zip ] || [ "$(realpath "$1")" != "$(realpath game.zip)" ]; then
        cp -f "$1" game.zip
    fi
fi
[ -f game.zip ] || {
    echo "error: game.zip not found; pass the path to your dump or copy it as game.zip" >&2
    exit 1
}
python3 - "$PWD/game.zip" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    names = z.namelist()
if "senxin_aleste_apm3.exe" not in names:
    sys.exit("error: game.zip does not contain senxin_aleste_apm3.exe")
if not any(n.startswith("windata/") for n in names):
    sys.exit("error: game.zip does not contain windata/")
PY

# 3. Build and install for the current user.
command -v flatpak-builder >/dev/null 2>&1 || {
    echo "error: flatpak-builder is not installed (inside the toolbox: sudo dnf install flatpak-builder)" >&2
    exit 1
}
exec flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
    build-dir io.github.senjinaleste.Recomp.json
