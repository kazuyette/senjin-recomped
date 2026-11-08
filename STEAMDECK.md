# Senjin Aleste on Steam Deck

You prepare the package once on a PC, copy one file to the Deck, then play from Gaming Mode.
Nothing needs to be installed on the Deck.

## 1. On your PC (Linux or WSL2)

You need your own copy of the game and the native build of this project (`./build.sh`, see README.md).

```
./play.sh deck /path/to/your/game/folder
```

This creates `out/deck/senjin-aleste-deck.tar` (a few GB). It contains the game binary, a static ffmpeg (for the
game's sound), a Japanese font, a copy of your game files and Deck settings (fullscreen, low input latency).

## 2. Copy it to the Steam Deck

1. Switch the Deck to **Desktop Mode** (Steam button > Power > Switch to Desktop).
2. Copy `senjin-aleste-deck.tar` to the Deck with a USB stick, an SD card or over the network,
   for example into your `Home` folder.
3. In the file manager (Dolphin), right-click the file > **Extract** > **Extract archive here**.
   You now have a `senjin-aleste-deck` folder. You can delete the `.tar` file.

## 3. Add it to Steam

1. Open **Steam** on the desktop.
2. **Games** > **Add a Non-Steam Game to My Library...** > **Browse...**
3. Set the file type filter to **All Files**, open the `senjin-aleste-deck` folder and pick **`aleste-deck.sh`**.
4. Click **Add Selected Programs**. You can rename the entry to "Senjin Aleste" in its Properties.

## 4. Play

Go back to **Gaming Mode** (desktop icon "Return to Gaming Mode") and start Senjin Aleste from your library.

| Action | Steam Deck |
|---|---|
| Move | D-pad or left stick |
| Shot / Power-up / Rapid / Blow | A / B / X / Y |
| Start | Start (menu button, right) |
| Pause | View button (left) |
| Quit | Steam button > Exit game |

The Deck's controls reach the game as a standard gamepad (Steam Input "Gamepad" layout), nothing to configure.

## Settings

Settings live in `senjin-aleste-deck/game/aleste.ini` (edit it in Desktop Mode with any text editor).
Useful keys: `[video] crt=0..3` (CRT filter), `nobezel=1` (playfield only, full height), `tate=1` (vertical mode),
`latency=2` (lowest input lag). Button choices: `BUTTON_01` to `BUTTON_04` in `[config]` (see README.md).

## Troubleshooting

If the game does not start, open `senjin-aleste-deck/deck.log` in Desktop Mode: it lists what went wrong
(for example a missing system library). Please include it when you open an issue.
