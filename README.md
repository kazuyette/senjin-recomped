# Senjin Aleste — native port

A native **Linux x86-64** build of the arcade shooter *Senjin Aleste* (M2, ALL.Net P-ras Multi Ver.3 / APM3 version),
made by **static recompilation** of the original Windows executable into C, plus a hand-written native system layer
(SDL2, OpenGL 3.3, FreeType). No Wine, no emulator, no interpreter.

It also provides a **Windows replacement for `apm_x86.dll`** so the original game runs on a normal Windows PC
(free play, offline, gamepad, CRT filter) without TeknoParrot.

> **Legal notice.** This repository contains **no part of the game**: no executable, no data, no generated code, no binaries
> built from it. You need your own copy of the game. The build scripts translate *your* executable on *your* machine.
> Senjin Aleste is © M2 / its respective owners. This project is not affiliated with or endorsed by them.
> Do not open issues asking where to get the game.

Version: see [`VERSION`](VERSION) and [`CHANGELOG.md`](CHANGELOG.md).

## Status
- Fully playable on Linux and WSL2 (WSLg): graphics, sound, music, voices, Japanese system texts, keyboard and gamepad (with rumble).
- Aime (player card) screens are skipped automatically.
- Options: playfield only without bezel (F8), CRT filter (F9), 60 Hz limiter.
- Windows: the original executable + this project's `apm_x86.dll` and launcher.

Tested with the executable whose SHA-256 is in [`recomp/exe.sha256`](recomp/exe.sha256) (title screen "VER. 1.02").
Other builds will not match `recomp/recomp_info.json`.

## Linux / WSL2

### 1. Build
Debian / Ubuntu:
```sh
sudo apt install build-essential pkg-config python3-pip libsdl2-dev libfreetype-dev libfontconfig-dev libgl-dev
pip install capstone pefile        # or: sudo apt install python3-capstone python3-pefile
./build.sh /path/to/game/senxin_aleste_apm3.exe
```
This generates ~30 MB of C in `recomp/gen/` (never commit or share it) and builds `out/senjin_aleste`.
The first build takes a few minutes; later builds only recompile the native layer.

### 2. Play
```sh
./play.sh deps                         # once: SDL2, ffmpeg, Mesa, a Japanese font (sudo)
./play.sh /path/to/game/folder         # the folder that contains senxin_aleste_apm3.exe and windata/
```
- **WSL2**: the game runs on your Windows GPU through Mesa d3d12 (`./play.sh info` shows the renderer).
  Games stored on a Windows drive (`/mnt/c/...`) are copied once to `~/.local/share/senjin-aleste/game` because Windows drives are slow from WSL2.
  WSL2 cannot see USB gamepads: build the Windows tools (below) and `play.sh` starts `out/windows/aleste_pad.exe`, a small XInput relay.
- **Linux**: the gamepad is read through SDL2 (XInput-compatible pads).
- `./play.sh <dir> --soft` forces software rendering, `-v` gives a verbose log (`senjin_aleste.log` in the game folder).

Sound and music are xWMA streams decoded with `ffmpeg` (cache in `~/.cache/senjin_aleste/xwma`).

## Windows
Build the tools with mingw-w64 (from Linux or WSL2):
```sh
sudo apt install mingw-w64
sh build_windows.sh            # -> out/windows/apm_x86.dll, launcher.exe, launcher.ini, aleste_pad.exe
```
In the game folder:
1. rename the original `apm_x86.dll` to `apm_x86.sega.dll` (keep it: put it back to return to the arcade setup);
2. copy `apm_x86.dll`, `launcher.exe` and `launcher.ini` there;
3. run `launcher.exe`, set your options and press Play.

The DLL is native Windows code: it provides the APM3 services (free play, offline ALL.Net, inputs, settings) and a Direct3D 9 overlay (CRT filter, vsync, playfield-only test).
Only 32-bit Windows binaries are built because the original game is 32-bit.

## Controls
| | Keyboard | Gamepad (default) |
|---|---|---|
| Move | Arrows | D-pad / left stick |
| Start / confirm | Enter | Start |
| Shot | Z | A |
| Power-up | X | B |
| Rapid shot | C | X |
| Blow | V | Y |
| Pause (the whole game, sound included) | P or Pause | Back |
| Fullscreen on/off | F11 or Alt+Enter | — |
| Bezel on/off | F8 | — |
| CRT filter level | F9 | — |
| Quit | Esc | — |

The left stick works exactly like the D-pad. Menus confirm with **Shot**. Gamepad buttons are set in the launcher (Controls tab: click an action, press a button).

## Settings: `aleste.ini`
Written by `launcher.exe` (Windows) in the game folder; on Linux you can edit it by hand (or run the launcher from WSL2).
Section `[config]` and the game's own `config.txt` are synchronised at start-up: a value changed in the launcher goes to the game, a value the game changed itself (it remembers its menu choices) comes back to `aleste.ini` and to the launcher. Main keys:

```ini
[config]
FULLSCREEN=0              ; also toggled in game with F11 / Alt+Enter (borderless, saved)
DISP_SCALE_X=300          ; 300 % or more for the CRT filter
DISP_SCALE_Y=300
LANGUAGE=1                ; 0 Japanese, 1 English
BUTTON_01=11              ; Shot      (launcher numbers: 1-4 D-pad, 5 Start, 6 Back, 9 LB, 10 RB, 11 A, 12 B, 13 X, 14 Y)
BUTTON_02=12              ; Power-up
BUTTON_03=13              ; Rapid shot
BUTTON_04=14              ; Blow
BUTTON_PAUSE=6
BUTTON_RESET=0
[video]
crt=0                     ; 0-3 (F9)
nobezel=0                 ; 1 = playfield only (F8)
[apm]
aime=0                    ; 1 = keep the Aime screens
dinput_pads=0             ; Windows: 1 = let the game read DirectInput (non-XInput) gamepads itself
```

The launcher follows the system language (English or French; `lang=en` / `lang=fr` in the `[launcher]` section of `launcher.ini` forces one). Translations live in `[lang.<code>]` sections of `launcher.ini`.

Known limitation of `nobezel=1`: texts drawn outside the playfield ("PRESS START BUTTON" in attract mode, system messages at the bottom) are hidden.

## Troubleshooting
| Variable | Effect |
|---|---|
| `SA_FPS=n` | frame limiter (default 60, 0 = off) |
| `SA_PERF=1` | time per frame in the log |
| `SA_NODRAW=1` | run without drawing (measures game logic) |
| `SA_AUDIO_LOG=1` | audio mixer log |
| `SA_XINPUT=1` | let the game read XInput itself (normally off: inputs go through the APM3 layer) |
| `SA_PADNET=port` | WSL2 gamepad relay port (default 47011) |
| `SA_FONT=path` | force a font file for Japanese texts |
| `SA_KEYTEST=1` | F1-F12 press raw arcade inputs 3-14 |

`kill -USR1 <pid>` prints a report of the Windows APIs called so far. Unhandled instructions or unknown indirect jumps are logged with their original address.

## How it works
```
senxin_aleste_apm3.exe ──x86recomp.py──▶ recomp/gen/*.c  (one C function per x86 function)
                                               │
native/ (KERNEL32, USER32, D3D9→OpenGL, XAudio2, GDI, DirectInput, APM3) ─┤──gcc──▶ out/senjin_aleste
kit/recomp_x86/x86rt.c (runtime helpers) ───────────────────────────────┘
```
- The game keeps its original 32-bit addresses inside a reserved 4 GB block; the PE image is loaded at 0x400000, imports point to native functions.
- CPU state (registers, flags, XMM, x87) is per-thread C state; indirect calls go through an address -> function table.
- Details: [`docs/recompilation.md`](docs/recompilation.md), [`docs/engine-map.md`](docs/engine-map.md), [`kit/recomp_x86/README.md`](kit/recomp_x86/README.md).

## Layout
| Path | Content |
|---|---|
| `native/` | native system layer (Linux) |
| `recomp/` | `recomp_info.json` (function boundaries / jump tables exported from Ghidra — addresses only), `gen.sh`, expected exe hash |
| `kit/recomp_x86/` | the x86 -> C generator and its runtime, Ghidra export script |
| `kit/crt/`, `kit/launcher/`, `kit/version/` | CRT filter, common launcher, version stamping |
| `src/apm3/` | Windows `apm_x86.dll` replacement and Direct3D 9 overlay |
| `src/pad/` | `aleste_pad.exe`, WSL2 gamepad relay |
| `tools/ghidra/` | analysis scripts |
| `docs/` | documentation and analysis data (addresses only) |

Source comments and some log messages are still in French.

## License
MIT for the code in this repository (see [`LICENSE`](LICENSE)). The game itself is not covered.
