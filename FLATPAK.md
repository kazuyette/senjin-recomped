# Flatpak (Fedora Silverblue / Bazzite)

Local-only Flatpak packaging for the native Linux port. The game files are
supplied by the user and are not part of the repository.

## Prerequisites

- A Flatpak host (Fedora Silverblue, Bazzite, ...).
- A toolbox with `flatpak-builder`:

  ```sh
  toolbox enter
  # inside the toolbox, if needed:
  sudo dnf install flatpak-builder
  ```

- The Freedesktop runtime and SDK 26.08. `build-flatpak.sh` installs them
  automatically with `flatpak install -y` if they are missing. To install them
  manually instead:

  ```sh
  flatpak install -y flathub \
      org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08
  ```

  If you prefer a per-user install, add Flathub to your user installation
  first and use `flatpak install -y --user flathub ...`.

- Your own game dump (the folder with `senxin_aleste_apm3.exe` and `windata/`),
  zipped.
- `curl` or `wget` for the first build: the script downloads the two Python
  wheels into `flatpak/wheels/`. No source snapshot is needed.

## Build

Inside `toolbox enter`, from the repository root:

```sh
./build-flatpak.sh "/path/to/your/dump.zip"
```

The script copies your dump to the generic name `game.zip`, checks its layout,
installs the Freedesktop runtime and SDK if they are missing, downloads the two
Python wheels if they are missing, and runs `flatpak-builder`. The manifest
uses this directory as its source tree
(`{"type": "dir", "path": "."}`), so it always builds the checkout you are
running from; no source snapshot is needed. If you prefer to run
`flatpak-builder` by hand, make sure `flatpak/wheels/` contains the two wheels
first (run `./build-flatpak.sh` once, or download them from the URLs in the
script), then:

```sh
cp "/path/to/your/dump.zip" game.zip
flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
    build-dir io.github.senjinaleste.Recomp.json
```

The manifest uses `game.zip`; no particular dump name is hardcoded in the
repository.

### Troubleshooting

- `Unable to get source file 'flatpak/wheels/...'`: run
  `./build-flatpak.sh` from the repository root; it downloads the wheels into
  `flatpak/wheels/`. Do not create `wheels/` at the repository root.
- `Can't load 'io.github.senjinaleste.Recomp.json'`: you are not in the
  repository root. `cd` to the directory that contains the manifest.
- `org.freedesktop.Sdk/x86_64/26.08 not installed` or `Unable to find sdk
  org.freedesktop.Sdk version 26.08`: `build-flatpak.sh` installs the runtime
  and SDK automatically, trying your existing user remotes first, then system
  remotes, and finally adding Flathub as a user remote. If that fails, install
  them manually:

  ```sh
  flatpak install -y flathub \
      org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08
  ```

  Or add Flathub to your user installation and install per-user:

  ```sh
  flatpak remote-add --if-not-exists --user flathub \
      https://flathub.org/repo/flathub.flatpakrepo
  flatpak install -y --user flathub \
      org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08
  ```

## Run

After installation, **Senjin Aleste** also appears in the desktop application
menu (GNOME, KDE, and other freedesktop-compliant environments) with a
placeholder gamepad icon. The menu entry runs the same
`senjin-aleste-launch` wrapper, so it uses the writable data directory
described below.

You can still start it from a terminal:

```sh
flatpak run io.github.senjinaleste.Recomp
```

If the menu entry does not appear immediately, log out and back in (or check
that the app is installed with `flatpak list --app`). The icon is a
placeholder; replace `flatpak/io.github.senjinaleste.Recomp.svg` with your own
SVG and rebuild if you want a different one.

## Flatseal / application details

The package ships AppStream metadata
(`flatpak/io.github.senjinaleste.Recomp.metainfo.xml`), so Flatseal and app
stores show the real name, developer, version and release date instead of the
app-ID fallback.

Flatseal may also list a **Background** portal permission. This is not
declared by the Flatpak manifest and the game does not need it; it is a
host-side xdg-desktop-portal permission. Turn it off in Flatseal, or remove
the stored permission:

```sh
flatpak permission-remove background background io.github.senjinaleste.Recomp
```

## Where the game writes

`/app` is read-only, but the game writes `config.txt`, `aleste.ini`,
`save.dat`, rankings and replays into its own folder. On first start the
launcher copies the game data to:

```
~/.var/app/io.github.senjinaleste.Recomp/data/senjin-aleste/game
```

It then `cd`s there and starts the game. The copy uses `cp -an`, so existing
settings and saves are never overwritten when the Flatpak is updated.

To reset the writable game data (keeping the Flatpak installed):

```sh
rm -rf ~/.var/app/io.github.senjinaleste.Recomp/data/senjin-aleste
```

## Permissions

- `--socket=wayland`, `--socket=fallback-x11`: Wayland, with X11 fallback.
- `--socket=pulseaudio`, `--device=dri`, `--device=input`: audio, GPU and
  gamepads.
- `--nofilesystem=home`, `--nofilesystem=host`, `--unshare=network`: no home,
  host or network access. The app's own `~/.var/app/...` directory remains
  writable.
- No external ffmpeg: the Freedesktop 26.08 runtime ships `ffmpeg`, which the
  game uses for xWMA decoding.
- Python build dependencies (`capstone`, `pefile`) are downloaded by
  `build-flatpak.sh` into `flatpak/wheels/` and installed with `--no-index`;
  the Flatpak build itself does not need network access.

## Japanese font

The runtime does not ship fonts; Flatpak exposes the host fonts through
fontconfig. If the game shows `????`, install a Japanese font on the host, for
example `google-noto-sans-cjk-fonts`:

```sh
# Silverblue/Bazzite
rpm-ostree install google-noto-sans-cjk-fonts
# or, without layering, copy a font to ~/.local/share/fonts and run fc-cache -f
```

Then restart the app.

## Legal

The Flatpak is for your own copy of the game. Do not distribute a bundle that
contains the game executable or `windata/`.
