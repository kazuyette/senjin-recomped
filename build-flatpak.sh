#!/bin/sh
# Build and install the Flatpak locally.
#
# Run this inside `toolbox enter` (the Fedora toolbox has flatpak-builder),
# from the repository root:
#
#   ./build-flatpak.sh "/path/to/your/game/dump.zip"
#
# or copy your dump to game.zip and run:
#
#   ./build-flatpak.sh
#
# The manifest uses this directory as its source tree and a generic game.zip
# for the game data: the repository never hardcodes the name of a particular
# dump, and no source snapshot is needed.  The Freedesktop Platform and SDK
# 26.08 are installed automatically if they are missing, and the Python build
# dependencies are downloaded into flatpak/wheels/ on the first run; the
# Flatpak build itself has no network access.
set -eu
cd "$(dirname "$0")"

# 1. Flatpak tooling.
command -v flatpak >/dev/null 2>&1 || {
    echo "error: flatpak is not installed" >&2
    exit 1
}
command -v flatpak-builder >/dev/null 2>&1 || {
    echo "error: flatpak-builder is not installed (inside the toolbox: sudo dnf install flatpak-builder)" >&2
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

# 3. The Freedesktop runtime and SDK required by the manifest.  Install them
#    if they are missing: use existing user remotes first, then system remotes
#    (Fedora usually configures its remotes system-wide), and add Flathub as a
#    user remote only as a last resort.
RUNTIME_REF="org.freedesktop.Platform//26.08"
SDK_REF="org.freedesktop.Sdk//26.08"

flatpak_ref_installed() {
    flatpak info "$1" >/dev/null 2>&1
}

flatpak_has_remote() {
    flatpak remotes "$1" --columns=name 2>/dev/null | grep -qx "$2"
}

install_flatpak_ref() {
    ref="$1"
    flatpak_ref_installed "$ref" && return 0

    # Prefer the user installation: it needs no root and is the same
    # installation flatpak-builder --user uses.
    for remote in flathub flathub-verified_floss flathub-floss fedora; do
        if flatpak_has_remote --user "$remote"; then
            echo "Installing $ref from $remote (user)..."
            if flatpak install -y --user "$remote" "$ref"; then
                flatpak_ref_installed "$ref" && return 0
            fi
        fi
    done

    # Fedora normally configures its remotes system-wide, and --user cannot
    # use a system remote.  Try the system installation next.
    for remote in flathub flathub-verified_floss flathub-floss fedora; do
        if flatpak_has_remote --system "$remote"; then
            echo "Installing $ref from $remote (system)..."
            if flatpak install -y "$remote" "$ref"; then
                flatpak_ref_installed "$ref" && return 0
            fi
        fi
    done

    # Last resort: add Flathub as a user remote and install there.
    if ! flatpak_has_remote --user flathub; then
        echo "Adding the Flathub remote (user)..."
        if flatpak remote-add --if-not-exists --user flathub \
                https://flathub.org/repo/flathub.flatpakrepo; then
            echo "Installing $ref from flathub (user)..."
            if flatpak install -y --user flathub "$ref"; then
                flatpak_ref_installed "$ref" && return 0
            fi
        fi
    fi

    echo "error: could not install $ref" >&2
    echo "  install it manually with:" >&2
    echo "    flatpak install -y flathub $RUNTIME_REF $SDK_REF" >&2
    echo "  (or add Flathub to your user installation and use --user)" >&2
    return 1
}

install_flatpak_ref "$RUNTIME_REF" || exit 1
install_flatpak_ref "$SDK_REF" || exit 1

# 4. Python build dependencies.  flatpak-builder needs them before it starts
#    the build, and the build sandbox has no network: fetch the exact wheels
#    once and keep them in flatpak/wheels/ (ignored by git).
WHEELS="flatpak/wheels"
CAPSTONE="capstone-5.0.9-py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl"
PEFILE="pefile-2024.8.26-py3-none-any.whl"
CAPSTONE_SHA256="273fd8d747d2e35c88f91450be51a603ecfaafb00d96d9f315dcb8689c86193e"
PEFILE_SHA256="76f8b485dcd3b1bb8166f1128d395fa3d87af26360c2358fb75b80019b957c6f"
CAPSTONE_URL="https://files.pythonhosted.org/packages/d1/39/17747862222bb062e86b501f1f148d5ff589b77908b080d30f7f085cbfb7/$CAPSTONE"
PEFILE_URL="https://files.pythonhosted.org/packages/54/16/12b82f791c7f50ddec566873d5bdd245baa1491bac11d15ffb98aecc8f8b/$PEFILE"

check_sha256() {
    [ -f "$1" ] || return 1
    command -v sha256sum >/dev/null 2>&1 || return 0
    [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]
}

fetch_wheel() {
    url="$1"; out="$2"; sha="$3"
    if check_sha256 "$out" "$sha"; then
        return 0
    fi
    rm -f "$out" "$out.part"
    echo "Downloading $(basename "$out")..."
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 -o "$out.part" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$out.part" "$url"
    else
        echo "error: need curl or wget to download $out" >&2
        exit 1
    fi
    mv -f "$out.part" "$out"
    if ! check_sha256 "$out" "$sha"; then
        echo "error: wrong sha256 for $out" >&2
        echo "  expected: $sha" >&2
        exit 1
    fi
}

mkdir -p "$WHEELS"
fetch_wheel "$CAPSTONE_URL" "$WHEELS/$CAPSTONE" "$CAPSTONE_SHA256"
fetch_wheel "$PEFILE_URL" "$WHEELS/$PEFILE" "$PEFILE_SHA256"

# 5. Build and install for the current user.
exec flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
    build-dir io.github.senjinaleste.Recomp.json
