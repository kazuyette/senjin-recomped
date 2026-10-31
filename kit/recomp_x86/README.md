# recomp_x86 — static recompilation of 32-bit x86 (Windows PE) to C

Generator version: see `VERSION` in `x86recomp.py`.

1. Ghidra (full analysis of the exe), then:
   `analyzeHeadless <project_dir> <name> -process <exe> -noanalysis -readOnly -scriptPath kit/recomp_x86/ghidra -postScript ExportRecompInfo.java recomp_info.json`
   -> functions (ranges), indirect jump targets (switches), IAT slots.
   For Senjin Aleste this file is already provided: `recomp/recomp_info.json`.
2. `python3 x86recomp.py <exe> recomp_info.json --out gen` -> `rc_NNN.c`, `rc_funcs.h`, `rc_table.c`, `rc_imports.c`, `rc_report.txt` (coverage).
3. Compile `gen/*.c` + `x86rt.c` + the project code (PE loading into `MEM`, native imports `x86_import_call`, platform layer).

Model: every function of the exe becomes `void f_XXXXXXXX(void)`; registers, flags, XMM and the x87 stack live in `cpu` (one copy per thread);
game memory is a 4 GB block `MEM` (original 32-bit addresses are kept: vtables, pointers, heap). Direct calls are C calls;
indirect calls/jumps go through `call_ind`/`jmp_ind` (sorted address -> function table, then imports). Unhandled instructions become `UNIMPL()` (the C still
compiles; `rc_report.txt` lists what to add). This is not an interpreter: everything is compiled ahead of time.

`x86_cpuid` reports an SSE2/SSE3 CPU without SSE4/AVX, so the MSVC CRT picks its SSE2 code paths.

Features: switch table guessing, orphan functions found through code pointers in data, import thunks, x87 (80-bit load/store, fnstenv/fldenv,
transcendentals), dead flag elimination (liveness from capstone `eflags`).

To do: MSVC C++ exceptions / SEH (`fs:[0]` chain).
