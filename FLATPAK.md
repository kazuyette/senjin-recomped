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

- The Freedesktop runtime and SDK 26.08:

  ```sh
  flatpak install flathub org.freedesktop.Platform//26.08 org.freedesktop.Sdk//26.08
  ```

- Your own game dump (the folder with `senxin_aleste_apm3.exe` and `windata/`),
  zipped.
- The source snapshot `senjin-recomped-0.6.21.zip` in the repository root. The
  manifest extracts it with `strip-components: 1`.

## Build

Inside `toolbox enter`, from the repository root:

```sh
./build-flatpak.sh "/path/to/your/dump.zip"
```

The script copies your dump to the generic name `game.zip`, checks its layout,
and runs `flatpak-builder`. You can also do it by hand:

```sh
cp "/path/to/your/dump.zip" game.zip
flatpak-builder --user --install --force-clean --disable-rofiles-fuse \
    build-dir io.github.senjinaleste.Recomp.json
```

The manifest uses `game.zip`; no particular dump name is hardcoded in the
repository.

The source snapshot is the release zip. If you modify the source tree,
regenerate `senjin-recomped-0.6.21.zip` before building, or change that source
to `{"type": "dir", "path": "."}` with a `skip` list for `data`, `*.zip`,
`build-dir` and `.flatpak-builder`.

## Run

```sh
flatpak run io.github.senjinaleste.Recomp
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
- `--share=ipc`: X11 MIT-SHM.
- `--nofilesystem=home`, `--nofilesystem=host`, `--unshare=network`: no home,
  host or network access. The app's own `~/.var/app/...` directory remains
  writable.
- No external ffmpeg: the Freedesktop 26.08 runtime ships `ffmpeg`, which the
  game uses for xWMA decoding.
- Python build dependencies (`capstone`, `pefile`) are vendored as wheels in
  `flatpak/wheels/`, so the build does not need network access.

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
