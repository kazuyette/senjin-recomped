# Why static recompilation

## The executable
- 6,675 functions, 1.64 MB of code, ~535,000 32-bit x86 instructions.
- Floating point mostly SSE (~34,600 instructions); little x87 (~1,900).
- ~3,700 indirect calls (C++ virtual methods, 205 vtables) and ~690 indirect jumps (switch tables, thunks).
- MSVC C++ exceptions / SEH in ~900 functions (`fs:[0]`).
- MSVC CRT and C++ library statically linked; embedded zlib, libcurl, AES, Vorbis.

## Options considered
| | A. Static recompilation x86 -> C | B. Manual decompilation to C++ | C. Native PE loader (keep the 32-bit exe) |
|---|---|---|---|
| Idea | A generator translates every function to C (registers = variables, 32-bit memory = a 4 GB block keeping original addresses); Windows calls land in our native layer | Rewrite the game from Ghidra, function by function | Load the exe as is in a 32-bit Linux process and provide our own 294 APIs |
| 64-bit | Yes | Yes | **No** (32-bit only; i386 is disappearing from distributions) |
| Time to first run | Medium | Very long | Short |
| Accuracy | Exact by construction (same instructions) | Risk of mistakes in every function | Exact |
| Performance | Good (optimising C compiler); some cost for indirections and memory base | Best | Original |
| No emulation | Yes (compiled code, CPU state in C variables, no interpreter) | Yes | Debatable: a mini-Wine |
| Can evolve | Yes: hot or system functions can later be rewritten by hand | — | No |

## Choice: A, then B progressively
1. **x86 -> C generator** (`kit/recomp_x86`, reusable for other Windows PC/arcade games), driven by Ghidra's function boundaries and jump tables.
2. **Hand-written native layer** for the `MWin*` classes, CRT/Windows APIs, APM3, XAudio2, GDI fonts (`native/`).
3. **Progressive rewriting** (B) of the functions worth it (rendering, main loop, loading), when needed.

## Status
- Generator 0.2.7: 7,300+ functions, ~467,000 instructions translated; the remaining unhandled instructions are SSE4/AVX CRT variants that are never taken (`cpuid` reports an SSE2/SSE3 CPU).
- Dead flag elimination removes ~52,000 flag computations.
- Not done yet: MSVC C++ exceptions / SEH (`fs:[0]` chain) — not needed by the paths the game takes so far; Windows 64-bit build of the native port (not planned: the Windows version keeps the original exe with our `apm_x86.dll`).

## Hard points met so far
- Indirect call ordering (target read before pushing the return address).
- Switch tables that Ghidra misses: guessed from `cmp reg, n ; ja` + `jmp [reg*4+table]`; switch cases must never be taken as function starts.
- Thread-safe game heap living inside the 4 GB block (`native/sa_mem.c`).
- x87: 80-bit loads/stores, environment save/restore, transcendental instructions used by the CRT math functions.
