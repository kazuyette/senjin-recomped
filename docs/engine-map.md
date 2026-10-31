# Engine map

Ghidra 12.1.2, full auto-analysis of `senxin_aleste_apm3.exe` (6,675 functions).
Scripts: `tools/ghidra/D3D9Map.java` (Direct3D 9 calls per function), `DecompileList.java` (targeted decompilation),
`ClassApiMap.java` (Windows APIs reached from each virtual method), `CallersOf.java`, `StrRefs.java`.
Data: `docs/data/functions.csv`, `d3d9_funcs.csv`, `d3d9_calls.csv` (addresses only, no game code).

## Engine: M2 "MLib", with RTTI
211 class names (`.?AV...`) are present in the executable. The ones that matter for the port:

- **Windows layer (replaced by the native layer):** `MWinApp`, `MWinAppTask`, `MWinD3D`, `MWinD3DWin`, `MWinInputConfig`, `MWinResCfg`, `MWinTxtCfg`, `MDiskApplicationDriver`, `MApplicationDriver`.
- Display: `MDisplay`, `MVirtualScreenDriver`, `MVirtualScreenView`, `MFixedSizeView`, `MView`, `D3DSceneRegion`, `SpriteDrawTask`, `MCapture`.
- Resources: `PSBObject`, `MTexPSBObject`, `MRawTex`, `MPSBFont`, `MEmbedFont`, `MFontCache`, `MResCfg`.
- Animation (.psb, E-mote/M2): `MMotion`, `MMotionPlayer`, `MMotionLayer`, `MMotionManager`, `MMotionRenderManager`, `MMotionResourceManager`, `MAnime`.
- Main loop / tasks: `MLooper` (+ VSync / pre / post hooks), `MTask`, `MThreadTask`, `MTaskSet`, `MParallelTaskSet`, `MSerialTaskSet`, `MBgTask`, `GameTask`.
- Sound: `MSound`, `MSoundLoadTask`, `MSoundFileLoad*Task`, `MSoundLoadArchiveTask`, `MSoundGroupVolumeAnime`.
- Input: `MInput`, `MInputHub`, `MSingleInputHub`, `MMouse`, `MTouchPad`, `MSoftKeyPad`.
- Arcade / network (neutralised): `AMDaemonManager@arcade`, `AbaasGs`, `Request/Task/HttpData@abaas@am`, curl, AES, zlib.

Windows dependencies are concentrated in the `MWin*` classes plus files, threads and sound, so the game and engine code can be kept as is (recompiled) and only the system layer rewritten.

## Rendering
- Game image: a **224x288** render target (A8R8G8B8) = the playfield only (title screen, sprites, the game's FPS counter).
- **1920x1080** render target (X8R8G8B8): final composition = scaled playfield + bezel art ("wall", 2048x2048 / 4096x2048 textures), then copied to the screen.
- 1 vertex shader + 2 pixel shaders (SM 2.0), ~40-75 `DrawPrimitiveUP` per frame (client-memory sprites, no vertex buffers).
- Engine inefficiency: ~2,400 `SetFVF`, ~2,250 `SetRenderState` and ~2,400 VS constant uploads per frame (no state caching).
- `DISP_WALLTYPE` (variable 0xecb338) only adds two layers to the bezel; the game has **no bezel-less mode**. The port's `[video] nobezel=1` draws the 224x288 target directly.
- Known limitation of `nobezel`: texts composed outside the playfield (attract "PRESS START BUTTON", system messages at the bottom) are not visible.

## What the executable asks from the system (294 APIs)
| Area | APIs | Engine classes | Native replacement |
|---|---|---|---|
| Rendering | `Direct3DCreate9` + IDirect3DDevice9; 6 d3dx9_43 math functions | `MWinD3D`, `MWinD3DWin`, `D3DSceneRegion`, `SpriteDrawTask` | OpenGL 3.3: SM 2.0 shaders translated to GLSL, `DrawPrimitiveUP` -> dynamic vertex buffer (`native/sa_d3d9.c`, `sa_d3dsm.c`) |
| Window / messages | USER32 (42 APIs), SHELL32 `DragAcceptFiles` | `MWinApp`, `MWinD3DWin` | SDL2 (`sa_user32.c`) |
| Input | XInput 1.3, `DirectInput8Create`, Raw Input; game buttons via APM3 `Input_isOn` | `MSingleInputHub`, `MInputHub`, `MWinInputConfig` | SDL2 keyboard / game controller, TCP relay under WSL2 (`sa_dinput.c`, `sa_misc.c`) |
| Sound | XAudio2 **2.7** through `CoCreateInstance`; xWMA / PCM / ADPCM in .psb archives | `MSound*` | Own 48 kHz mixer on SDL audio, xWMA decoded with ffmpeg (`sa_xaudio2.c`) |
| Text | GDI `CreateFontIndirectA`, `GetGlyphOutlineA`, `GetTextMetricsA`; Shift-JIS conversions | `MEmbedFont` | FreeType + fontconfig, real cp932 tables (`sa_gdi.c`) |
| Files | `CreateFileA/W`, `ReadFile` (incl. overlapped), `FindFirstFileExW`... | `MDiskFileReadTask`, `MWinTxtCfg` | POSIX (`sa_k32.c`) |
| Threads / sync | `CreateThread`, events, mutexes, semaphores, critical sections, TLS | `MThreadTask`, `MLooper*`, `MBgTask` | pthreads (`sa_k32.c`) |
| Time | `QueryPerformanceCounter`, `timeGetTime`, `Sleep` | `MLooperClock` | `clock_gettime` / SDL |
| Arcade / network | APM3 (45 of 106 exports used), WS2_32, curl, bcrypt, WMI | `AMDaemonManager`, `AbaasGs` | Free play, offline; APM3 services rewritten natively (`sa_misc.c`, `src/apm3`) |
| MSVC CRT | ~90 KERNEL32 APIs (heap, locale, console) | C runtime | Recompiled CRT on top of the native KERNEL32 layer; game heap in `sa_mem.c` |

## Notable addresses
- `Input_isOn(n+1)`: n = 1-4 directions, 5 Start, 6-13 arcade buttons 1-8 (menus confirm with arcade button 1, n = 6).
- Aime state machine: `FUN_00433190`, state at `0x8efa30` (2 = "use Aime?", 0xf = "start game"), cursor at `0x8efa28`.
- `MEmbedFont`: `FUN_0052c870` / `FUN_0052d0d0` (WideCharToMultiByte CP 932, then `GetGlyphOutlineA` GGO_BITMAP).
- Keychip check: `0x53f180` expects an 11-character serial at `System_getKeychipId()+0x22`; game id `SBRF`.
