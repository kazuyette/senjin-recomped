# Changelog

## v0.6.7 — all game settings in the launcher
- New launcher tabs Modes (story and score attack choices), Training and Training+ (starting conditions): every setting of the game's config.txt is now in the launcher.
- Two-way sync between aleste.ini and config.txt (native and Windows DLL): the game rewrites config.txt itself (menu cursors), so a value changed in the game is copied back to aleste.ini unless it was changed in the launcher since the previous start (`[config.sync]` keeps the values applied last time). Settings missing from aleste.ini are taken from config.txt, by the game at startup and by the launcher (`[launcher] seed=config/config.txt`, new common launcher option).
- play.sh copies aleste.ini back to the game folder after playing from the WSL2 copy.

## v0.6.6 — crash at the end of stage 3 (Linux)
- "game heap exhausted" after ~15 min of play: textures released by the game were never freed (their CPU copies stayed in the game heap, 16-32 MB each), the CPU copy of every texture was kept after upload, and blocks of 128 KB+ were rounded up to the next power of 2 (thousands of 128 KB blocks became 256 KB: 300 MB lost at startup).
- Fixes: textures freed on their last Release (GL objects deleted by the render thread), CPU copy only while locked (read back from the GPU for a later lock), size classes up to 32 KB then 4 KB granularity, heap starting right after the thread stacks (1.9 GB instead of 1.76). Heap after 3000 frames: 1.4 GB -> 0.86 GB, stable over long runs. The log now shows heap and texture counts every 600 frames.
- Thread stack/TEB slots of finished threads are reused; `SystemTimeToFileTime` implemented (last missing import).

## v0.6.5 — pause, stick fixes
- Pause: the launcher's "Pause" button (default Back) or P / Pause freezes the whole game (Linux and Windows): game clocks stand still, sound stops, the image is dimmed (Linux) and the window title shows "PAUSE". Same button to resume. The game's own pause input had no effect and is no longer sent.
- Left stick = D-pad. Linux (SDL): stick fully up was read as fully down (16-bit overflow). Windows: the game's own DirectInput-to-XInput layer (meant for non-XInput pads) could read the pad on top of our inputs, a likely cause of the inverted Y axis [hypothesis, to be confirmed]; game controllers are now hidden from it (`[apm] dinput_pads=1` to restore).

## v0.6.4 — English code base, robustness fixes
- All comments, log messages and tool output are now in English. The common launcher has an English UI by default (`[launcher] lang=fr` for French).
- Native layer: closed handles are recycled (no more 4096-handle limit), D3D9 surfaces per texture level are cached, the backbuffer is reused on Reset, `D3DADAPTER_IDENTIFIER9.VendorId` offset fixed, bounds checks on shader registers and lock levels, static-area limit fixed and no more per-call growth (`FindNextFile`, `GetModuleFileNameW`...), `BCryptDecrypt` output size checked, XAudio2 callbacks of destroyed voices dropped, decode cache accounting fixed, dead duplicate GDI functions removed.
- Recompiler: SHL carry flag no longer relies on undefined C shifts for 8/16-bit operands; shifts/rotates by CL no longer let the dead-flag pass drop flags still needed (count may be 0); `fninit` sets FCW to 0x37F like hardware.
- Windows DLL: bounded config buffers and checked file I/O; render-target references held correctly (released before Reset); log format fixes.
- Gamepad relay: proper disconnect detection and complete 16-byte sends.
- Launcher / extractor: buffer bounds, terminated conversions, 64-bit file offsets for disc images over 2 GB. Ghidra export: full JSON string escaping.

## v0.6.3 — first public release
- Public repository: build scripts take your own `senxin_aleste_apm3.exe` (checked by SHA-256), generate the C locally and build `out/senjin_aleste`; `play.sh` runs it on Linux / WSL2.
- English launcher, English documentation.
- Launcher default gamepad buttons fixed (A / B / X / Y / Back, matching the built-in defaults).

## v0.6.2 — playfield-only mode + kit CRT filter (native), performance
- `[video] nobezel=1` (F8) shows the 224x288 playfield only, integer scaled; `[video] crt=0..3` (F9) applies the kit CRT filter ported to GLSL (same constants and formulas as `kit/crt`).
- `capture.req` grabs the window output (CRT included).
- Performance: `SA_PERF=1` / `SA_NODRAW=1` show the recompiled game logic is cheap and rendering dominates; GLSL uniform locations cached per program.
- Generator 0.2.7: dead flag elimination (~52,000 flag computations removed).

## v0.6.1 — Japanese system texts, Aime screen skipped (native and Windows DLL)
- System texts showed "????": `MEmbedFont` converts each character to Shift-JIS and draws it with `GetGlyphOutlineA`. Fixed with real cp932 tables and GDI fonts on FreeType + fontconfig.
- "Closing in 0 min" message: `ApmSystemSetting_getTimeToClosingTime` must write -1.
- Aime screens skipped automatically (state machine `FUN_00433190`); `[apm] aime=1` keeps them.
- Windows DLL: same gamepad mapping as the native build; the game's own XInput reading is disabled so buttons are not doubled.

## v0.5.8 — freeze on first contact (real cause)
- A switch case had been taken as a function start (`f_0047b007`), corrupting callee-saved registers. Generator 0.2.6: switch targets are never function starts, jump-table bytes are no longer read as immediates.
- Rumble: `XInputSetState` drives SDL or the WSL2 relay.

## v0.5.7 — gamepad buttons set in the launcher
- Native `aleste.ini [config]` -> `config.txt` sync (the DLL does it on Windows).

## v0.5.6 — freeze on enemy contact, gamepad under WSL2
- Generator 0.2.5: switch tables `jmp [reg*4+table]` guessed from `cmp reg, n ; ja`.
- `aleste_pad.exe`: Windows XInput relay over localhost TCP (WSL2 cannot see USB gamepads).

## v0.5.5 — keyboard and Esc (native)
- Arrows, Enter = Start, Z X C V = buttons, Esc = quit.

## v0.5.4 — Start works
- Generator 0.2.4: x87 `fxam`, `fscale`, `fyl2x`, `fpatan`, `f2xm1`, `fprem1`, `ficom`, `fucompi`; `xlatb`, `loope/loopne`, `bt*` with memory operands, `pmaxsw/pminsw`.

## v0.5.3 — WSL2 on a real GPU
- Mesa d3d12 under WSLg; 60 Hz frame limiter (`SA_FPS`) because WSLg vsync does not pace.

## v0.5.2 / v0.5.1 — first runs on real Linux machines
- Generator 0.2.2: `fnstenv/fldenv`, `fnsave/frstor`, 80-bit `fld/fstp`.
- SIGINT/SIGTERM exit immediately.

## v0.5.0 — first native frame (Linux 64-bit, recompiled code)
- Generator 0.2.1: indirect call target read before pushing the return address.
- Native XAudio2 2.7, PE resources from `.rsrc`, thread-safe game heap.

## v0.4.x — recompilation starts
- `kit/recomp_x86` generator + Ghidra export; all generated C compiles and links on Linux x86-64.
- Native layer: 4 GB address space, PE loader, KERNEL32 layer; the MSVC CRT start-up runs.

## v0.1 – v0.3 — Windows DLL and analysis
- `apm_x86.dll`: native replacement for the arcade APM3 services (free play, offline, inputs, config sync) — the game runs on Windows without TeknoParrot.
- Direct3D 9 overlay in the DLL: kit CRT filter, vsync and FPS options, statistics / shader dump, playfield-only test.
- Common launcher with gamepad button capture.
- Ghidra mapping of the M2 "MLib" engine.
