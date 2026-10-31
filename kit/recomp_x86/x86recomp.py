#!/usr/bin/env python3
"""x86recomp.py - kit recomp_x86 : recompilation statique d'un exe x86 32 bits (PE) en C.

Entrées : l'exe + le JSON de Ghidra (ExportRecompInfo.java : fonctions, cibles des sauts indirects, IAT).
Sorties (dossier --out) :
  rc_funcs.h          prototypes
  rc_NNN.c            fonctions traduites (découpées en lots)
  rc_table.c          table adresse -> fonction (triée, pour call_ind / jmp_ind)
  rc_imports.c        table des emplacements d'IAT -> nom d'import
  rc_report.txt       couverture : instructions traduites / non gérées (par mnémonique)
Usage : python3 x86recomp.py jeu.exe recomp_info.json --out gen [--per-file 250]
Chaque fonction de l'exe devient `void f_XXXXXXXX(void)` ; registres et drapeaux dans `cpu` (x86rt.h).
Les instructions non encore gérées deviennent UNIMPL(adresse, "texte") : le C compile toujours, le
rapport dit quoi ajouter. Pas d'émulation : c'est du code C compilé.
"""
import argparse, collections, json, os, re, sys
import pefile
import capstone
from capstone import x86_const as X

VERSION = "0.2.7"

REG32 = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"}
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
CC = {"o": "CC_O", "no": "CC_NO", "b": "CC_B", "ae": "CC_AE", "e": "CC_E", "ne": "CC_NE", "be": "CC_BE", "a": "CC_A",
      "s": "CC_S", "ns": "CC_NS", "p": "CC_P", "np": "CC_NP", "l": "CC_L", "ge": "CC_GE", "le": "CC_LE", "g": "CC_G"}


# ---------- élimination des drapeaux morts ----------
# Un calcul de drapeaux (fl_add, cpu.cf = ...) est retiré quand, en suivant le chemin séquentiel, tous les drapeaux
# qu'il écrit sont réécrits avant d'être lus (ou que vient un call / ret, après lesquels ils ne servent plus).
# Prudence : on ne retire que pour une liste d'instructions dont la traduction ne fait qu'écrire les drapeaux.
_FL = ("CF", "ZF", "SF", "OF", "PF")
def _fmask(ins, kinds):
    m = 0
    for i, f in enumerate(_FL):
        for k in kinds:
            v = getattr(X, f"X86_EFLAGS_{k}_{f}", 0)
            if v and (ins.eflags & v): m |= 1 << i
    return m
def _freads(ins):
    if ins.mnemonic in ("pushfd", "pushf", "lahf", "into") or ins.mnemonic.startswith("fcmov"): return 31
    return _fmask(ins, ("TEST",))
def _fwrites(ins): return _fmask(ins, ("MODIFY", "RESET", "SET", "UNDEFINED"))
_STRIPPABLE = {"add", "sub", "and", "or", "xor", "cmp", "test", "inc", "dec", "neg", "shl", "sal", "shr", "sar",
               "rol", "ror", "imul", "mul", "adc", "sbb", "bt"}
_RE_FL = re.compile(r"fl_\w+\([^;]*\);\s*")
_RE_FA = re.compile(r"cpu\.(cf|of|zf|sf|pf|af) = [^;]*;\s*")

class Unsupported(Exception):
    pass


class Gen:
    def __init__(self, pe, info):
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.funcs = {}  # entry -> (name, ranges)
        for f in info["functions"]:
            e = int(f["entry"], 16)
            self.funcs[e] = (f["name"], [(int(a, 16), int(b, 16)) for a, b in f["ranges"]])
        self.jumps = {int(k, 16): [int(t, 16) for t in v] for k, v in info["jumps"].items()}
        self.imports = {int(k, 16): v for k, v in info["imports"].items()}
        self.stats = collections.Counter()
        # thunks d'import « jmp dword ptr [IAT] » (Ghidra ne les exporte pas comme fonctions)
        self.thunks = {}
        text = [s for s in pe.sections if s.Name.startswith(b".text")][0]
        tdata = text.get_data(); tva = self.base + text.VirtualAddress
        i = tdata.find(b"\xff\x25")
        while i != -1:
            slot = int.from_bytes(tdata[i + 2:i + 6], "little")
            if slot in self.imports and (tva + i) not in self.funcs: self.thunks[tva + i] = slot
            i = tdata.find(b"\xff\x25", i + 1)
        # tables de sauts « jmp [reg*4 + table] » : entrées devinées (Ghidra n'a pas toujours défini la fonction),
        # et adresses des tables exclues des débuts de fonction supposés
        import bisect, struct
        self.text_lo, self.text_hi = tva, tva + len(tdata)
        self.tables = set()
        i = tdata.find(b"\xff\x24")
        while i != -1:
            if i + 7 <= len(tdata) and tdata[i + 2] in (0x85, 0x8D, 0x95, 0x9D, 0xAD, 0xB5, 0xBD):
                ja = tva + i; disp = int.from_bytes(tdata[i + 3:i + 7], "little")
                tg = self.guess_table(ja, disp)
                if tg:
                    for k in range(len(tg)): self.tables.add(disp + 4 * k)
                    if ja not in self.jumps: self.jumps[ja] = tg
            i = tdata.find(b"\xff\x24", i + 1)
        # cibles de sauts connues (cas de switch) : ce sont des étiquettes, jamais des débuts de fonction
        jtargets = set(t for v in self.jumps.values() for t in v)
        # pointeurs de code trouvés dans les données, hors de toute fonction connue -> fonctions supplémentaires
        rr = sorted(r for _, (_, rg) in self.funcs.items() for r in rg); st = [r[0] for r in rr]
        def inside(a):
            k = bisect.bisect_right(st, a) - 1
            return k >= 0 and rr[k][0] <= a <= rr[k][1]
        known = sorted(set(self.funcs) | set(self.thunks))
        extra = set()
        for sec in pe.sections:
            if sec.Name.startswith((b".rdata", b".data")):
                d = sec.get_data()
                for k in range(0, len(d) - 3, 4):
                    v = struct.unpack_from("<I", d, k)[0]
                    if tva <= v < tva + len(tdata) and v not in self.funcs and v not in self.thunks and v not in jtargets and not inside(v): extra.add(v)
        # immédiats dans le code (push offset fonction, mov reg, offset) : début de fonction probable si aligné / après bourrage
        for k in range(0, len(tdata) - 3):
            v = int.from_bytes(tdata[k:k + 4], "little")
            if (tva + k) in self.tables or (tva + k - 1) in self.tables or (tva + k - 2) in self.tables or (tva + k - 3) in self.tables: continue
            if tva <= v < tva + len(tdata) and v not in self.funcs and v not in self.thunks and v not in extra and v not in self.tables and v not in jtargets and not inside(v):
                prev = tdata[v - tva - 1] if v > tva else 0xCC
                if v % 16 == 0 or prev in (0xCC, 0xC3, 0xC2, 0x90):
                    extra.add(v)
        for e in sorted(extra):
            j = bisect.bisect_right(known, e)
            end = (known[j] - 1) if j < len(known) else tva + len(tdata) - 1
            self.funcs[e] = (f"orphan_{e:08x}", [(e, end)])
        self.n_extra = len(extra)
        self.unimpl = collections.Counter()

    def guess_table(self, ja, disp):
        """entrées d'une table de sauts : pointeurs dans .text, proches du saut ; borne par « cmp reg, n ; ja » si présent"""
        if not (self.text_lo <= disp < self.text_hi + 0x100000): return []
        bound = 256
        pre = self.read(ja - 6, 6)          # cmp eax, imm8 (83 F8 ib) ; ja rel8 (77 xx)
        if len(pre) == 6 and pre[4] == 0x77 and pre[1] == 0x83 and (pre[2] & 0xF8) == 0xF8: bound = pre[3] + 1
        pre = self.read(ja - 9, 9)          # cmp reg, imm8 ; ja rel32 (0F 87)
        if bound == 256 and len(pre) == 9 and pre[0] == 0x83 and (pre[1] & 0xF8) == 0xF8 and pre[3] == 0x0F and pre[4] == 0x87: bound = pre[2] + 1
        out = []
        for k in range(bound):
            b = self.read(disp + 4 * k, 4)
            if len(b) < 4: break
            v = int.from_bytes(b, "little")
            if not (self.text_lo <= v < self.text_hi and abs(v - ja) < 0x10000): break
            out.append(v)
        return out

    # ---------- lecture du binaire ----------
    def read(self, addr, n):
        try:
            return self.pe.get_data(addr - self.base, n)
        except Exception:
            return b""

    def in_func(self, ranges, a):
        return any(lo <= a <= hi for lo, hi in ranges)

    # ---------- opérandes ----------
    def reg_get(self, name):
        if name in REG32: return f"cpu.{name}"
        if name in REG16: return f"LO16(cpu.{REG16[name]})"
        if name in REG8L: return f"LO8(cpu.{REG8L[name]})"
        if name in REG8H: return f"HI8(cpu.{REG8H[name]})"
        raise Unsupported("reg " + name)

    def reg_set(self, name, v):
        if name in REG32: return f"cpu.{name} = (uint32_t)({v});"
        if name in REG16: return f"SETLO16(cpu.{REG16[name]}, {v});"
        if name in REG8L: return f"SETLO8(cpu.{REG8L[name]}, {v});"
        if name in REG8H: return f"SETHI8(cpu.{REG8H[name]}, {v});"
        raise Unsupported("reg " + name)

    def ea(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_get(ins.reg_name(m.base)) if ins.reg_name(m.base) in REG32 else self._bad(ins.reg_name(m.base)))
        if m.index:
            r = self.reg_get(ins.reg_name(m.index))
            parts.append(f"{r} * {m.scale}" if m.scale != 1 else r)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xFFFFFFFF:x}u")
        e = " + ".join(parts)
        if m.segment == X.X86_REG_FS:
            e = f"cpu.fs_base + {e}"
        elif m.segment not in (0, X.X86_REG_DS, X.X86_REG_SS, X.X86_REG_ES, X.X86_REG_CS):
            raise Unsupported("segment " + ins.reg_name(m.segment))
        return f"(uint32_t)({e})"

    def _bad(self, r):
        raise Unsupported("base " + r)

    def get(self, ins, op, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            return self.reg_get(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & MASKS[bits]:x}u"
        if op.type == X.X86_OP_MEM:
            return f"rd{bits}({self.ea(ins, op)})"
        raise Unsupported("op")

    def put(self, ins, op, v, bits=None):
        bits = bits or op.size * 8
        if op.type == X.X86_OP_REG:
            return self.reg_set(ins.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            return f"wr{bits}({self.ea(ins, op)}, ({uint(bits)})({v}));"
        raise Unsupported("dst")

    # ---------- traduction d'une instruction ----------
    def tr(self, ins, ctx):
        m = ins.mnemonic
        ops = ins.operands
        a = ins.address
        nxt = a + ins.size
        prefix = ins.prefix[0] if ins.prefix else 0
        if m.startswith("lock "): m = m[5:]
        m = m.replace("bnd ", "").replace("notrack ", "")
        g, p = self.get, self.put

        if m in ("nop", "pause", "endbr32", "fwait", "wait", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "lfence", "mfence", "sfence"):
            return ""
        if m == "int3": return f"x86_int3(0x{a:x});"
        if m == "mov":
            return p(ins, ops[0], g(ins, ops[1]))
        if m in ("movzx", "movsx"):
            sb = ops[1].size * 8
            v = g(ins, ops[1])
            if m == "movsx": v = f"(int32_t)(int{sb}_t){v}"
            return p(ins, ops[0], v)
        if m == "lea":
            return p(ins, ops[0], self.ea(ins, ops[1]), 32 if ops[0].size == 4 else 16)
        if m == "push":
            if ops[0].size == 2: raise Unsupported("push16")
            return f"{{ uint32_t _v = {g(ins, ops[0], 32)}; push32(_v); }}"
        if m == "pop":
            return f"{{ uint32_t _v = pop32(); {p(ins, ops[0], '_v', 32)} }}"
        if m == "pushal" or m == "pushad":
            return "{ uint32_t _s = cpu.esp; push32(cpu.eax); push32(cpu.ecx); push32(cpu.edx); push32(cpu.ebx); push32(_s); push32(cpu.ebp); push32(cpu.esi); push32(cpu.edi); }"
        if m == "popal" or m == "popad":
            return "{ cpu.edi = pop32(); cpu.esi = pop32(); cpu.ebp = pop32(); pop32(); cpu.ebx = pop32(); cpu.edx = pop32(); cpu.ecx = pop32(); cpu.eax = pop32(); }"
        if m == "pushfd":
            return "push32(cpu.cf | (cpu.pf << 2) | (cpu.zf << 6) | (cpu.sf << 7) | (cpu.df << 10) | (cpu.of << 11) | 0x202);"
        if m == "popfd":
            return "{ uint32_t _f = pop32(); cpu.cf = _f & 1; cpu.pf = (_f >> 2) & 1; cpu.zf = (_f >> 6) & 1; cpu.sf = (_f >> 7) & 1; cpu.df = (_f >> 10) & 1; cpu.of = (_f >> 11) & 1; }"
        if m == "leave":
            return "cpu.esp = cpu.ebp; cpu.ebp = pop32();"
        if m in ("add", "adc", "sub", "sbb", "cmp"):
            bits = ops[0].size * 8
            c = "cpu.cf" if m in ("adc", "sbb") else "0"
            s = f"{{ uint32_t _a = {g(ins, ops[0])}, _b = {sext(g(ins, ops[1], bits), ops[1], bits)}, _c = {c}; "
            if m in ("add", "adc"):
                s += f"fl_add(_a, _b, _c, {bits}); {p(ins, ops[0], '_a + _b + _c')} }}"
            elif m == "cmp":
                s += f"fl_sub(_a, _b, 0, {bits}); }}"
            else:
                s += f"fl_sub(_a, _b, _c, {bits}); {p(ins, ops[0], '_a - _b - _c')} }}"
            return s
        if m in ("and", "or", "xor", "test"):
            bits = ops[0].size * 8
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            if m == "xor" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG and ops[0].reg == ops[1].reg:
                return f"{p(ins, ops[0], '0')} fl_logic(0, {bits});"
            s = f"{{ uint32_t _r = {g(ins, ops[0])} {op} {sext(g(ins, ops[1], bits), ops[1], bits)}; fl_logic(_r, {bits}); "
            if m != "test": s += p(ins, ops[0], "_r")
            return s + " }"
        if m in ("inc", "dec"):
            bits = ops[0].size * 8
            o = "+" if m == "inc" else "-"
            return f"{{ uint32_t _a = {g(ins, ops[0])}, _r = _a {o} 1; fl_incdec(_r, _a, {int(m == 'inc')}, {bits}); {p(ins, ops[0], '_r')} }}"
        if m == "neg":
            bits = ops[0].size * 8
            return f"{{ uint32_t _a = {g(ins, ops[0])}; fl_sub(0, _a, 0, {bits}); {p(ins, ops[0], '0u - _a')} }}"
        if m == "not":
            return p(ins, ops[0], f"~{g(ins, ops[0])}")
        if m in ("shl", "sal", "shr", "sar", "rol", "ror"):
            bits = ops[0].size * 8
            cnt = g(ins, ops[1], 8) if len(ops) > 1 else "1"
            s = f"{{ uint32_t _n = ({cnt}) & 31; if (_n) {{ uint32_t _a = {g(ins, ops[0])} & {MASKS[bits]:#x}u, _r; "
            if m in ("shl", "sal"):
                s += f"_r = (uint32_t)((uint64_t)_a << _n); cpu.cf = (uint32_t)(((uint64_t)_a << _n) >> {bits}) & 1; fl_logic(_r, {bits}); cpu.cf = ((uint64_t)_a >> ({bits} - _n)) & 1; cpu.of = MSB(_r, {bits}) ^ cpu.cf; "
            elif m == "shr":
                s += f"_r = _a >> _n; fl_logic(_r, {bits}); cpu.cf = (_a >> (_n - 1)) & 1; cpu.of = MSB(_a, {bits}); "
            elif m == "sar":
                s += f"_r = (uint32_t)((int32_t)(int{bits}_t)_a >> (_n > {bits - 1} ? {bits - 1} : _n)); fl_logic(_r, {bits}); cpu.cf = ((int32_t)(int{bits}_t)_a >> (_n - 1)) & 1; cpu.of = 0; "
            elif m == "rol":
                s += f"_n %= {bits}; _r = _n ? ((_a << _n) | (_a >> ({bits} - _n))) : _a; cpu.cf = _r & 1; cpu.of = MSB(_r, {bits}) ^ cpu.cf; "
            else:
                s += f"_n %= {bits}; _r = _n ? ((_a >> _n) | (_a << ({bits} - _n))) : _a; cpu.cf = MSB(_r, {bits}); cpu.of = MSB(_r, {bits}) ^ ((_r >> ({bits} - 2)) & 1); "
            return s + p(ins, ops[0], "_r") + " } }"
        if m in ("rcr", "rcl") and ops[0].size == 4:
            cnt = g(ins, ops[1], 8) if len(ops) > 1 else "1"
            if m == "rcr":
                body = "uint64_t _v = ((uint64_t)cpu.cf << 32) | _a; for (uint32_t _i = 0; _i < _n; _i++) { uint64_t _lo = _v & 1; _v = (_v >> 1) | (_lo << 32); } "
            else:
                body = "uint64_t _v = ((uint64_t)cpu.cf << 32) | _a; for (uint32_t _i = 0; _i < _n; _i++) { uint64_t _hi = (_v >> 32) & 1; _v = ((_v << 1) | _hi) & 0x1FFFFFFFFull; } "
            return f"{{ uint32_t _n = ({cnt}) & 31; uint32_t _a = {g(ins, ops[0])}; {body} cpu.cf = (uint32_t)(_v >> 32) & 1; {p(ins, ops[0], '(uint32_t)_v')} }}"
        if m in ("shld", "shrd"):
            cnt = g(ins, ops[2], 8)
            if ops[0].size != 4: raise Unsupported(m + "16")
            if m == "shld":
                e = "(uint32_t)((((uint64_t)_a << 32) | _b) << _n >> 32)"
                cf = "(_a >> (32 - _n)) & 1"
            else:
                e = "(uint32_t)((((uint64_t)_b << 32) | _a) >> _n)"
                cf = "(_a >> (_n - 1)) & 1"
            return f"{{ uint32_t _n = ({cnt}) & 31; if (_n) {{ uint32_t _a = {g(ins, ops[0])}, _b = {g(ins, ops[1])}, _r = {e}; fl_logic(_r, 32); cpu.cf = {cf}; {p(ins, ops[0], '_r')} }} }}"
        if m == "imul":
            if len(ops) == 1:
                bits = ops[0].size * 8
                if bits == 32:
                    return f"{{ int64_t _r = (int64_t)(int32_t)cpu.eax * (int32_t){g(ins, ops[0])}; cpu.eax = (uint32_t)_r; cpu.edx = (uint32_t)(_r >> 32); cpu.cf = cpu.of = (_r != (int32_t)_r); }}"
                if bits == 16:
                    return f"{{ int32_t _r = (int16_t)LO16(cpu.eax) * (int16_t){g(ins, ops[0])}; SETLO16(cpu.eax, _r); SETLO16(cpu.edx, _r >> 16); cpu.cf = cpu.of = (_r != (int16_t)_r); }}"
                return f"{{ int16_t _r = (int8_t)LO8(cpu.eax) * (int8_t){g(ins, ops[0])}; SETLO16(cpu.eax, _r); cpu.cf = cpu.of = (_r != (int8_t)_r); }}"
            bits = ops[0].size * 8
            sa, sb = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
            return (f"{{ int64_t _r = (int64_t)(int{bits}_t){g(ins, sa, bits)} * (int64_t)(int{bits}_t){sext(g(ins, sb, bits), sb, bits)}; "
                    f"cpu.cf = cpu.of = (_r != (int{bits}_t)_r); {p(ins, ops[0], '(uint32_t)_r')} }}")
        if m == "mul":
            bits = ops[0].size * 8
            if bits == 32:
                return f"{{ uint64_t _r = (uint64_t)cpu.eax * {g(ins, ops[0])}; cpu.eax = (uint32_t)_r; cpu.edx = (uint32_t)(_r >> 32); cpu.cf = cpu.of = cpu.edx != 0; }}"
            if bits == 16:
                return f"{{ uint32_t _r = (uint32_t)LO16(cpu.eax) * {g(ins, ops[0])}; SETLO16(cpu.eax, _r); SETLO16(cpu.edx, _r >> 16); cpu.cf = cpu.of = (_r >> 16) != 0; }}"
            return f"{{ uint32_t _r = (uint32_t)LO8(cpu.eax) * {g(ins, ops[0])}; SETLO16(cpu.eax, _r); cpu.cf = cpu.of = (_r >> 8) != 0; }}"
        if m in ("div", "idiv"):
            bits = ops[0].size * 8
            if bits != 32: raise Unsupported(m + str(bits))
            if m == "div":
                return f"{{ uint64_t _n = ((uint64_t)cpu.edx << 32) | cpu.eax; uint32_t _d = {g(ins, ops[0])}; cpu.eax = (uint32_t)(_n / _d); cpu.edx = (uint32_t)(_n % _d); }}"
            return f"{{ int64_t _n = (int64_t)(((uint64_t)cpu.edx << 32) | cpu.eax); int32_t _d = (int32_t){g(ins, ops[0])}; cpu.eax = (uint32_t)(int32_t)(_n / _d); cpu.edx = (uint32_t)(int32_t)(_n % _d); }}"
        if m == "cdq": return "cpu.edx = (cpu.eax & 0x80000000u) ? 0xFFFFFFFFu : 0;"
        if m == "cwde": return "cpu.eax = (uint32_t)(int32_t)(int16_t)LO16(cpu.eax);"
        if m == "cbw": return "SETLO16(cpu.eax, (int16_t)(int8_t)LO8(cpu.eax));"
        if m == "cwd": return "SETLO16(cpu.edx, (LO16(cpu.eax) & 0x8000) ? 0xFFFF : 0);"
        if m == "xchg":
            return f"{{ uint32_t _a = {g(ins, ops[0])}, _b = {g(ins, ops[1])}; {p(ins, ops[0], '_b')} {p(ins, ops[1], '_a')} }}"
        if m == "xadd":
            bits = ops[0].size * 8
            return f"{{ uint32_t _a = {g(ins, ops[0])}, _b = {g(ins, ops[1])}; fl_add(_a, _b, 0, {bits}); {p(ins, ops[1], '_a')} {p(ins, ops[0], '_a + _b')} }}"
        if m == "cmpxchg":
            bits = ops[0].size * 8
            acc = {32: "cpu.eax", 16: "LO16(cpu.eax)", 8: "LO8(cpu.eax)"}[bits]
            seta = {32: "cpu.eax = _d;", 16: "SETLO16(cpu.eax, _d);", 8: "SETLO8(cpu.eax, _d);"}[bits]
            return f"{{ uint32_t _d = {g(ins, ops[0])}; fl_sub({acc}, _d, 0, {bits}); if (cpu.zf) {{ {p(ins, ops[0], g(ins, ops[1]))} }} else {{ {seta} }} }}"
        if m in ("bt", "bts", "btr", "btc"):
            bits = ops[0].size * 8
            if ops[0].type == X.X86_OP_MEM and ops[1].type == X.X86_OP_REG:   # chaîne de bits : adresse + (n >> 3) signé
                op_ = {"bt": "", "bts": "_v | (1u << (_n & 7))", "btr": "_v & ~(1u << (_n & 7))", "btc": "_v ^ (1u << (_n & 7))"}[m]
                wb = f" wr8(_ad, (uint8_t)({op_}));" if op_ else ""
                return f"{{ int32_t _n = (int32_t)({g(ins, ops[1], 32)}); uint32_t _ad = {self.ea(ins, ops[0])} + (uint32_t)(_n >> 3); uint32_t _v = rd8(_ad); cpu.cf = (_v >> (_n & 7)) & 1;{wb} }}"
            s = f"{{ uint32_t _a = {g(ins, ops[0])}, _n = ({g(ins, ops[1], bits)}) & {bits - 1}; cpu.cf = (_a >> _n) & 1; "
            if m == "bts": s += p(ins, ops[0], "_a | (1u << _n)")
            if m == "btr": s += p(ins, ops[0], "_a & ~(1u << _n)")
            if m == "btc": s += p(ins, ops[0], "_a ^ (1u << _n)")
            return s + " }"
        if m in ("bsf", "bsr"):
            fn = "__builtin_ctz(_a)" if m == "bsf" else "31 - __builtin_clz(_a)"
            return f"{{ uint32_t _a = {g(ins, ops[1])}; cpu.zf = _a == 0; if (_a) {{ {p(ins, ops[0], fn)} }} }}"
        if m == "bswap":
            return p(ins, ops[0], f"__builtin_bswap32({g(ins, ops[0])})")
        if m.startswith("set") and m[3:] in CC:
            return p(ins, ops[0], f"({CC[m[3:]]}) ? 1 : 0")
        if m.startswith("cmov") and m[4:] in CC:
            return f"if ({CC[m[4:]]}) {{ {p(ins, ops[0], g(ins, ops[1]))} }}"
        if m in ("cld", "std"): return f"cpu.df = {int(m == 'std')};"
        if m in ("clc", "stc", "cmc"): return {"clc": "cpu.cf = 0;", "stc": "cpu.cf = 1;", "cmc": "cpu.cf ^= 1;"}[m]
        if m == "sahf": return "{ uint32_t _f = HI8(cpu.eax); cpu.cf = _f & 1; cpu.pf = (_f >> 2) & 1; cpu.zf = (_f >> 6) & 1; cpu.sf = (_f >> 7) & 1; }"
        if m == "lahf": return "SETHI8(cpu.eax, cpu.cf | (cpu.pf << 2) | (cpu.zf << 6) | (cpu.sf << 7) | 2);"
        r = self.tr_string(ins, m)
        if r is not None: return r
        r = self.tr_sse(ins, m, ops)
        if r is not None: return r
        r = self.tr_x87(ins, m, ops)
        if r is not None: return r
        if m == "cpuid": return "x86_cpuid();"
        if m == "rdtsc": return "x86_rdtsc();"
        raise Unsupported(m)

    def tr_string(self, ins, m):
        base = m.replace("rep ", "").replace("repe ", "").replace("repne ", "").replace("repz ", "").replace("repnz ", "")
        kinds = {"movsb": 1, "movsw": 2, "movsd": 4, "stosb": 1, "stosw": 2, "stosd": 4, "lodsb": 1, "lodsd": 4,
                 "cmpsb": 1, "cmpsd": 4, "scasb": 1, "scasw": 2, "scasd": 4}
        if base not in kinds: return None
        if base in ("movsd", "cmpsd") and len(ins.operands) == 2 and any(o.type == X.X86_OP_REG and ins.reg_name(o.reg).startswith("xmm") for o in ins.operands):
            return None  # movsd SSE
        n = kinds[base]; bits = n * 8
        st = "(cpu.df ? -" + str(n) + " : " + str(n) + ")"
        acc_get = {1: "LO8(cpu.eax)", 2: "LO16(cpu.eax)", 4: "cpu.eax"}[n]
        acc_set = {1: "SETLO8(cpu.eax, _v);", 2: "SETLO16(cpu.eax, _v);", 4: "cpu.eax = _v;"}[n]
        op = base[:4]
        if op == "movs": body = f"wr{bits}(cpu.edi, rd{bits}(cpu.esi)); cpu.esi += {st}; cpu.edi += {st};"
        elif op == "stos": body = f"wr{bits}(cpu.edi, {acc_get}); cpu.edi += {st};"
        elif op == "lods": body = f"{{ uint32_t _v = rd{bits}(cpu.esi); {acc_set} }} cpu.esi += {st};"
        elif op == "cmps": body = f"fl_sub(rd{bits}(cpu.esi), rd{bits}(cpu.edi), 0, {bits}); cpu.esi += {st}; cpu.edi += {st};"
        else: body = f"fl_sub({acc_get}, rd{bits}(cpu.edi), 0, {bits}); cpu.edi += {st};"
        if m.startswith("rep ") and op in ("movs", "stos", "lods"):
            return f"while (cpu.ecx) {{ {body} cpu.ecx--; }}"
        if m.startswith(("repe ", "repz ")):
            return f"while (cpu.ecx) {{ {body} cpu.ecx--; if (!cpu.zf) break; }}"
        if m.startswith(("repne ", "repnz ")):
            return f"while (cpu.ecx) {{ {body} cpu.ecx--; if (cpu.zf) break; }}"
        if m.startswith("rep"): raise Unsupported(m)
        return body

    # ---------- SSE ----------
    def x(self, ins, op):
        return int(ins.reg_name(op.reg)[3:])

    def is_xmm(self, ins, op):
        return op.type == X.X86_OP_REG and ins.reg_name(op.reg).startswith("xmm")

    def tr_sse(self, ins, m, ops):
        g, p = self.get, self.put
        def xr(op): return f"cpu.xmm[{self.x(ins, op)}]"
        def src128(op):
            if self.is_xmm(ins, op): return f"xmm_t _s = {xr(op)};"
            return f"xmm_t _s; rd128({self.ea(ins, op)}, &_s);"
        if m in ("movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu"):
            if self.is_xmm(ins, ops[0]): return f"{{ {src128(ops[1])} {xr(ops[0])} = _s; }}"
            return f"wr128({self.ea(ins, ops[0])}, &{xr(ops[1])});"
        if m in ("movss", "movsd") and (self.is_xmm(ins, ops[0]) or self.is_xmm(ins, ops[1])):
            sz = 32 if m == "movss" else 64
            fld = "d[0]" if sz == 32 else "q[0]"
            if self.is_xmm(ins, ops[0]) and self.is_xmm(ins, ops[1]):
                return f"{xr(ops[0])}.{fld} = {xr(ops[1])}.{fld};"
            if self.is_xmm(ins, ops[0]):
                return f"{{ xmm_t _z = {{0}}; _z.{fld} = rd{sz}({self.ea(ins, ops[1])}); {xr(ops[0])} = _z; }}"
            return f"wr{sz}({self.ea(ins, ops[0])}, {xr(ops[1])}.{fld});"
        if m in ("movd", "movq"):
            sz = 32 if m == "movd" else 64
            fld = "d[0]" if sz == 32 else "q[0]"
            if self.is_xmm(ins, ops[0]):
                if self.is_xmm(ins, ops[1]): return f"{{ xmm_t _z = {{0}}; _z.q[0] = {xr(ops[1])}.q[0]; {xr(ops[0])} = _z; }}"
                v = g(ins, ops[1], sz) if ops[1].type == X.X86_OP_REG else f"rd{sz}({self.ea(ins, ops[1])})"
                return f"{{ xmm_t _z = {{0}}; _z.{fld} = {v}; {xr(ops[0])} = _z; }}"
            if ops[0].type == X.X86_OP_REG: return p(ins, ops[0], f"{xr(ops[1])}.d[0]")
            return f"wr{sz}({self.ea(ins, ops[0])}, {xr(ops[1])}.{fld});"
        if m in ("movhps", "movlps", "movhpd", "movlpd"):
            i = 1 if m[3] == "h" else 0
            if self.is_xmm(ins, ops[0]): return f"{xr(ops[0])}.q[{i}] = rd64({self.ea(ins, ops[1])});"
            return f"wr64({self.ea(ins, ops[0])}, {xr(ops[1])}.q[{i}]);"
        if m == "movlhps": return f"{xr(ops[0])}.q[1] = {xr(ops[1])}.q[0];"
        if m == "movhlps": return f"{xr(ops[0])}.q[0] = {xr(ops[1])}.q[1];"
        # scalaires
        sc = {"addss": "+", "subss": "-", "mulss": "*", "divss": "/", "addsd": "+", "subsd": "-", "mulsd": "*", "divsd": "/"}
        if m in sc or m in ("minss", "maxss", "minsd", "maxsd", "sqrtss", "sqrtsd"):
            single = m.endswith("ss"); t = "f" if single else "g"; sz = 32 if single else 64
            b = f"{xr(ops[1])}.{t}[0]" if self.is_xmm(ins, ops[1]) else f"rdf{sz}({self.ea(ins, ops[1])})"
            d = f"{xr(ops[0])}.{t}[0]"
            if m in sc: return f"{d} = {d} {sc[m]} {b};"
            if m.startswith("min"): return f"{{ {'float' if single else 'double'} _b = {b}; {d} = ({d} < _b) ? {d} : _b; }}"
            if m.startswith("max"): return f"{{ {'float' if single else 'double'} _b = {b}; {d} = ({d} > _b) ? {d} : _b; }}"
            return f"{d} = {'sqrtf' if single else 'sqrt'}({b});"
        pk = {"addps": ("f", 4, "+"), "subps": ("f", 4, "-"), "mulps": ("f", 4, "*"), "divps": ("f", 4, "/"),
              "addpd": ("g", 2, "+"), "subpd": ("g", 2, "-"), "mulpd": ("g", 2, "*"), "divpd": ("g", 2, "/"),
              "andps": ("d", 4, "&"), "andpd": ("d", 4, "&"), "orps": ("d", 4, "|"), "orpd": ("d", 4, "|"),
              "xorps": ("d", 4, "^"), "xorpd": ("d", 4, "^"), "pand": ("d", 4, "&"), "por": ("d", 4, "|"), "pxor": ("d", 4, "^"),
              "paddd": ("d", 4, "+"), "psubd": ("d", 4, "-"), "paddq": ("q", 2, "+"), "psubq": ("q", 2, "-"),
              "paddw": ("w", 8, "+"), "psubw": ("w", 8, "-"), "paddb": ("b", 16, "+"), "psubb": ("b", 16, "-")}
        if m in pk:
            t, n, o = pk[m]
            if m in ("xorps", "xorpd", "pxor") and self.is_xmm(ins, ops[1]) and ops[0].reg == ops[1].reg:
                return f"memset(&{xr(ops[0])}, 0, 16);"
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < {n}; _i++) {xr(ops[0])}.{t}[_i] = {xr(ops[0])}.{t}[_i] {o} _s.{t}[_i]; }}"
        if m in ("andnps", "andnpd", "pandn"):
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < 4; _i++) {xr(ops[0])}.d[_i] = ~{xr(ops[0])}.d[_i] & _s.d[_i]; }}"
        if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
            single = m.endswith("ss"); t = "f" if single else "g"; sz = 32 if single else 64
            b = f"{xr(ops[1])}.{t}[0]" if self.is_xmm(ins, ops[1]) else f"rdf{sz}({self.ea(ins, ops[1])})"
            return f"ucomis_set({xr(ops[0])}.{t}[0], {b});"
        if m in ("cvtsi2ss", "cvtsi2sd"):
            t = "f" if m.endswith("ss") else "g"
            return f"{xr(ops[0])}.{t}[0] = ({'float' if t == 'f' else 'double'})(int32_t){g(ins, ops[1], 32)};"
        if m in ("cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si"):
            single = "ss" in m; t = "f" if single else "g"; sz = 32 if single else 64
            b = f"{xr(ops[1])}.{t}[0]" if self.is_xmm(ins, ops[1]) else f"rdf{sz}({self.ea(ins, ops[1])})"
            fn = "cvtt_i32" if "tt" in m else "cvt_i32"
            return p(ins, ops[0], f"(uint32_t){fn}({b})", 32)
        if m == "cvtss2sd":
            b = f"{xr(ops[1])}.f[0]" if self.is_xmm(ins, ops[1]) else f"rdf32({self.ea(ins, ops[1])})"
            return f"{xr(ops[0])}.g[0] = (double){b};"
        if m == "cvtsd2ss":
            b = f"{xr(ops[1])}.g[0]" if self.is_xmm(ins, ops[1]) else f"rdf64({self.ea(ins, ops[1])})"
            return f"{xr(ops[0])}.f[0] = (float){b};"
        if m == "cvtdq2ps": return f"{{ {src128(ops[1])} for (int _i = 0; _i < 4; _i++) {xr(ops[0])}.f[_i] = (float)(int32_t)_s.d[_i]; }}"
        if m == "cvttps2dq": return f"{{ {src128(ops[1])} for (int _i = 0; _i < 4; _i++) {xr(ops[0])}.d[_i] = (uint32_t)cvtt_i32(_s.f[_i]); }}"
        if m == "cvtps2pd": return f"{{ {src128(ops[1])} {xr(ops[0])}.g[1] = _s.f[1]; {xr(ops[0])}.g[0] = _s.f[0]; }}"
        if m == "cvtpd2ps": return f"{{ {src128(ops[1])} {xr(ops[0])}.f[0] = (float)_s.g[0]; {xr(ops[0])}.f[1] = (float)_s.g[1]; {xr(ops[0])}.q[1] = 0; }}"
        if m == "cvtdq2pd": return f"{{ {src128(ops[1])} {xr(ops[0])}.g[1] = (int32_t)_s.d[1]; {xr(ops[0])}.g[0] = (int32_t)_s.d[0]; }}"
        if m == "unpcklps": return f"{{ {src128(ops[1])} xmm_t _d = {xr(ops[0])}; {xr(ops[0])}.d[0] = _d.d[0]; {xr(ops[0])}.d[1] = _s.d[0]; {xr(ops[0])}.d[2] = _d.d[1]; {xr(ops[0])}.d[3] = _s.d[1]; }}"
        if m == "unpcklpd" or m == "punpcklqdq": return f"{{ {src128(ops[1])} {xr(ops[0])}.q[1] = _s.q[0]; }}"
        if m == "unpckhpd" or m == "punpckhqdq": return f"{{ {src128(ops[1])} {xr(ops[0])}.q[0] = {xr(ops[0])}.q[1]; {xr(ops[0])}.q[1] = _s.q[1]; }}"
        if m == "shufps":
            imm = ops[2].imm
            return (f"{{ {src128(ops[1])} xmm_t _d = {xr(ops[0])}; {xr(ops[0])}.d[0] = _d.d[{imm & 3}]; {xr(ops[0])}.d[1] = _d.d[{(imm >> 2) & 3}]; "
                    f"{xr(ops[0])}.d[2] = _s.d[{(imm >> 4) & 3}]; {xr(ops[0])}.d[3] = _s.d[{(imm >> 6) & 3}]; }}")
        if m == "pshufd":
            imm = ops[2].imm
            return f"{{ {src128(ops[1])} " + " ".join(f"{xr(ops[0])}.d[{i}] = _s.d[{(imm >> (2 * i)) & 3}];" for i in range(4)) + " }"
        if m in ("sqrtps", "maxps", "minps"):
            if m == "sqrtps": return f"{{ {src128(ops[1])} for (int _i = 0; _i < 4; _i++) {xr(ops[0])}.f[_i] = sqrtf(_s.f[_i]); }}"
            c = ">" if m == "maxps" else "<"
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < 4; _i++) {xr(ops[0])}.f[_i] = ({xr(ops[0])}.f[_i] {c} _s.f[_i]) ? {xr(ops[0])}.f[_i] : _s.f[_i]; }}"
        if m in ("psrlq", "psllq", "psrld", "pslld", "psrad", "psrldq", "pslldq") and ops[1].type == X.X86_OP_IMM:
            n = ops[1].imm; r = xr(ops[0])
            if m == "psrlq": return f"for (int _i = 0; _i < 2; _i++) {r}.q[_i] = {n} > 63 ? 0 : {r}.q[_i] >> {n};"
            if m == "psllq": return f"for (int _i = 0; _i < 2; _i++) {r}.q[_i] = {n} > 63 ? 0 : {r}.q[_i] << {n};"
            if m == "psrld": return f"for (int _i = 0; _i < 4; _i++) {r}.d[_i] = {n} > 31 ? 0 : {r}.d[_i] >> {n};"
            if m == "pslld": return f"for (int _i = 0; _i < 4; _i++) {r}.d[_i] = {n} > 31 ? 0 : {r}.d[_i] << {n};"
            if m == "psrad": return f"for (int _i = 0; _i < 4; _i++) {r}.d[_i] = (uint32_t)((int32_t){r}.d[_i] >> ({n} > 31 ? 31 : {n}));"
            if m == "psrldq": return f"{{ xmm_t _d = {{0}}; if ({n} < 16) memcpy(_d.b, {r}.b + {n}, 16 - {n}); {r} = _d; }}"
            return f"{{ xmm_t _d = {{0}}; if ({n} < 16) memcpy(_d.b + {n}, {r}.b, 16 - {n}); {r} = _d; }}"
        if m == "movmskps": return p(ins, ops[0], f"({xr(ops[1])}.d[0] >> 31) | (({xr(ops[1])}.d[1] >> 31) << 1) | (({xr(ops[1])}.d[2] >> 31) << 2) | (({xr(ops[1])}.d[3] >> 31) << 3)", 32)
        if m == "movmskpd": return p(ins, ops[0], f"({xr(ops[1])}.q[0] >> 63) | (({xr(ops[1])}.q[1] >> 63) << 1)", 32)
        import re as _re
        mm = _re.fullmatch(r"cmp(eq|lt|le|unord|neq|nlt|nle|ord)(ss|sd|ps|pd)", m)
        if mm:
            pr, ty = mm.groups()
            n, t, it = {"ss": (1, "f", "d"), "sd": (1, "g", "q"), "ps": (4, "f", "d"), "pd": (2, "g", "q")}[ty]
            cmpe = {"eq": "_a == _b", "lt": "_a < _b", "le": "_a <= _b", "unord": "(_a != _a || _b != _b)", "neq": "!(_a == _b)",
                    "nlt": "!(_a < _b)", "nle": "!(_a <= _b)", "ord": "(_a == _a && _b == _b)"}[pr]
            ones = "0xFFFFFFFFu" if it == "d" else "0xFFFFFFFFFFFFFFFFull"
            if self.is_xmm(ins, ops[1]): ld = f"xmm_t _s = {xr(ops[1])};"
            elif ty in ("ss", "sd"): ld = f"xmm_t _s = {{0}}; _s.{it}[0] = rd{32 if ty == 'ss' else 64}({self.ea(ins, ops[1])});"
            else: ld = f"xmm_t _s; rd128({self.ea(ins, ops[1])}, &_s);"
            return (f"{{ {ld} for (int _i = 0; _i < {n}; _i++) {{ {'float' if t == 'f' else 'double'} _a = {xr(ops[0])}.{t}[_i], _b = _s.{t}[_i]; "
                    f"{xr(ops[0])}.{it}[_i] = ({cmpe}) ? {ones} : 0; }} }}")
        if m in ("pcmpeqd", "pcmpeqb", "pcmpeqw", "pcmpgtd"):
            t, n = {"pcmpeqd": ("d", 4), "pcmpgtd": ("d", 4), "pcmpeqb": ("b", 16), "pcmpeqw": ("w", 8)}[m]
            c = "(int32_t)" if m == "pcmpgtd" else ""
            op = ">" if m == "pcmpgtd" else "=="
            ones = {"d": "0xFFFFFFFFu", "w": "0xFFFF", "b": "0xFF"}[t]
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < {n}; _i++) {xr(ops[0])}.{t}[_i] = ({c}{xr(ops[0])}.{t}[_i] {op} {c}_s.{t}[_i]) ? {ones} : 0; }}"
        if m in ("pshuflw", "pshufhw"):
            imm = ops[2].imm; base = 0 if m == "pshuflw" else 4
            return f"{{ {src128(ops[1])} xmm_t _d = _s; " + " ".join(f"_d.w[{base + i}] = _s.w[{base + ((imm >> (2 * i)) & 3)}];" for i in range(4)) + f" {xr(ops[0])} = _d; }}"
        if m in ("punpckhbw", "punpckhwd", "punpckhdq"):
            t, n = {"punpckhbw": ("b", 16), "punpckhwd": ("w", 8), "punpckhdq": ("d", 4)}[m]
            return f"{{ {src128(ops[1])} xmm_t _d = {xr(ops[0])}; for (int _i = 0; _i < {n // 2}; _i++) {{ {xr(ops[0])}.{t}[2 * _i] = _d.{t}[{n // 2} + _i]; {xr(ops[0])}.{t}[2 * _i + 1] = _s.{t}[{n // 2} + _i]; }} }}"
        if m in ("pminub", "pmaxub", "pavgb", "psubusb", "paddusb"):
            e = {"pminub": "_a < _b ? _a : _b", "pmaxub": "_a > _b ? _a : _b", "pavgb": "(_a + _b + 1) >> 1", "psubusb": "_a > _b ? _a - _b : 0", "paddusb": "_a + _b > 255 ? 255 : _a + _b"}[m]
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < 16; _i++) {{ int _a = {xr(ops[0])}.b[_i], _b = _s.b[_i]; {xr(ops[0])}.b[_i] = (uint8_t)({e}); }} }}"
        if m in ("pmaxsw", "pminsw"):
            e = "_a > _b ? _a : _b" if m == "pmaxsw" else "_a < _b ? _a : _b"
            return f"{{ {src128(ops[1])} for (int _i = 0; _i < 8; _i++) {{ int16_t _a = (int16_t){xr(ops[0])}.w[_i], _b = (int16_t)_s.w[_i]; {xr(ops[0])}.w[_i] = (uint16_t)({e}); }} }}"
        if m == "pmovmskb":
            return p(ins, ops[0], f"({{ uint32_t _m = 0; for (int _i = 0; _i < 16; _i++) _m |= (uint32_t)({xr(ops[1])}.b[_i] >> 7) << _i; _m; }})", 32)
        if m == "pextrw":
            return p(ins, ops[0], f"{xr(ops[1])}.w[{ops[2].imm & 7}]", 32)
        if m == "pinsrw":
            v = self.get(ins, ops[1], 16) if ops[1].type == X.X86_OP_MEM else f"(uint16_t){self.get(ins, ops[1], 32)}"
            return f"{xr(ops[0])}.w[{ops[2].imm & 7}] = {v};"
        if m in ("psllq", "psrlq", "pslld", "psrld") and self.is_xmm(ins, ops[1]):
            t, bits, d = ("q", 64, "<<") if m == "psllq" else ("q", 64, ">>") if m == "psrlq" else ("d", 32, "<<") if m == "pslld" else ("d", 32, ">>")
            n = 2 if t == "q" else 4
            return f"{{ uint64_t _n = {xr(ops[1])}.q[0]; for (int _i = 0; _i < {n}; _i++) {xr(ops[0])}.{t}[_i] = _n > {bits - 1} ? 0 : {xr(ops[0])}.{t}[_i] {d} _n; }}"
        if m in ("punpcklbw", "punpcklwd", "punpckldq"):
            t, n = {"punpcklbw": ("b", 16), "punpcklwd": ("w", 8), "punpckldq": ("d", 4)}[m]
            return f"{{ {src128(ops[1])} xmm_t _d = {xr(ops[0])}; for (int _i = 0; _i < {n // 2}; _i++) {{ {xr(ops[0])}.{t}[2 * _i] = _d.{t}[_i]; {xr(ops[0])}.{t}[2 * _i + 1] = _s.{t}[_i]; }} }}"
        if m in ("stmxcsr", "ldmxcsr"):
            return f"wr32({self.ea(ins, ops[0])}, 0x1F80);" if m == "stmxcsr" else ""
        return None

    # ---------- x87 ----------
    def fsrc(self, ins, op):
        if op.type == X.X86_OP_REG:
            return f"ST({self.streg(ins, op)})"
        if op.size == 4: return f"(double)rdf32({self.ea(ins, op)})"
        if op.size == 8: return f"rdf64({self.ea(ins, op)})"
        if op.size == 10: return f"rdf80({self.ea(ins, op)})"
        raise Unsupported("x87 taille " + str(op.size))

    def streg(self, ins, op):
        n = ins.reg_name(op.reg)  # st(0)..st(7) ; capstone : "st(1)"
        return int(n[3]) if n.startswith("st(") else 0

    def tr_x87(self, ins, m, ops):
        if not m.startswith("f"): return None
        ea = lambda op: self.ea(ins, op)
        if m == "fld":
            return f"fpush({self.fsrc(ins, ops[0])});" if ops[0].type == X.X86_OP_MEM else f"{{ double _v = ST({self.streg(ins, ops[0])}); fpush(_v); }}"
        if m == "fild":
            t = {2: "int16_t", 4: "int32_t", 8: "int64_t"}[ops[0].size]
            return f"fpush((double)({t})rd{ops[0].size * 8}({ea(ops[0])}));"
        if m in ("fst", "fstp"):
            pop = " fpop();" if m == "fstp" else ""
            if ops[0].type == X.X86_OP_REG: return f"ST({self.streg(ins, ops[0])}) = ST(0);{pop}"
            if ops[0].size == 4: return f"wrf32({ea(ops[0])}, (float)ST(0));{pop}"
            if ops[0].size == 8: return f"wrf64({ea(ops[0])}, ST(0));{pop}"
            if ops[0].size == 10: return f"wrf80({ea(ops[0])}, ST(0));{pop}"
            raise Unsupported("fstp80")
        if m in ("fist", "fistp", "fisttp"):
            sz = ops[0].size * 8
            cv = "trunc" if m == "fisttp" else "nearbyint"
            pop = " fpop();" if m != "fist" else ""
            return f"wr{sz}({ea(ops[0])}, (uint{sz}_t)(int{sz}_t){cv}(ST(0)));{pop}"
        ar = {"fadd": "+", "fsub": "-", "fmul": "*", "fdiv": "/", "fsubr": "r-", "fdivr": "r/"}
        base = m[:-1] if m.endswith("p") and m[:-1] in ar else m
        if base in ar or base in ("fiadd", "fisub", "fimul", "fidiv", "fisubr", "fidivr"):
            o = ar.get(base) or ar[base.replace("fi", "f")]
            rev = o.startswith("r"); o = o[-1]
            if base.startswith("fi"):
                t = {2: "int16_t", 4: "int32_t"}[ops[0].size]
                b = f"(double)({t})rd{ops[0].size * 8}({ea(ops[0])})"
                return f"ST(0) = {'(' + b + ') ' + o + ' ST(0)' if rev else 'ST(0) ' + o + ' (' + b + ')'};"
            if len(ops) == 0:  # faddp sans opérande = st(1) op= st(0), pop
                return f"ST(1) = {'ST(0) ' + o + ' ST(1)' if rev else 'ST(1) ' + o + ' ST(0)'}; fpop();"
            if len(ops) == 1:
                b = self.fsrc(ins, ops[0])
                return f"ST(0) = {'(' + b + ') ' + o + ' ST(0)' if rev else 'ST(0) ' + o + ' (' + b + ')'};"
            d, s = self.streg(ins, ops[0]), self.streg(ins, ops[1])
            e = f"ST({s}) {o} ST({d})" if rev else f"ST({d}) {o} ST({s})"
            return f"ST({d}) = {e};" + (" fpop();" if m.endswith("p") else "")
        if m == "fchs": return "ST(0) = -ST(0);"
        if m == "fabs": return "ST(0) = fabs(ST(0));"
        if m == "fsqrt": return "ST(0) = sqrt(ST(0));"
        if m == "fsin": return "ST(0) = sin(ST(0));"
        if m == "fcos": return "ST(0) = cos(ST(0));"
        if m == "frndint": return "ST(0) = nearbyint(ST(0));"
        if m == "fxam": return "fxam_set(ST(0));"
        if m == "fscale": return "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"
        if m == "fyl2x": return "{ double _v = ST(1) * log2(ST(0)); fpop(); ST(0) = _v; }"
        if m == "fyl2xp1": return "{ double _v = ST(1) * log2(1.0 + ST(0)); fpop(); ST(0) = _v; }"
        if m == "fpatan": return "{ double _v = atan2(ST(1), ST(0)); fpop(); ST(0) = _v; }"
        if m == "fptan": return "ST(0) = tan(ST(0)); fpush(1.0); cpu.fsw &= ~0x400;"
        if m == "fsincos": return "{ double _a = ST(0); ST(0) = sin(_a); fpush(cos(_a)); cpu.fsw &= ~0x400; }"
        if m == "f2xm1": return "ST(0) = exp2(ST(0)) - 1.0;"
        if m == "fprem1": return "ST(0) = remainder(ST(0), ST(1)); cpu.fsw &= ~0x400;"
        if m == "fldlg2": return "fpush(0.30102999566398119521);"
        if m == "fldl2t": return "fpush(3.32192809488736234787);"
        if m in ("ficom", "ficomp"):
            t = {2: "int16_t", 4: "int32_t"}[ops[0].size]
            return f"fcom_set(ST(0), (double)({t})rd{ops[0].size * 8}({ea(ops[0])}));" + (" fpop();" if m == "ficomp" else "")
        if m == "fldz": return "fpush(0.0);"
        if m == "fld1": return "fpush(1.0);"
        if m == "fldpi": return "fpush(3.14159265358979323846);"
        if m == "fldl2e": return "fpush(1.44269504088896340736);"
        if m == "fldln2": return "fpush(0.69314718055994530942);"
        if m == "fxch":
            i = self.streg(ins, ops[0]) if ops else 1
            return f"{{ double _t = ST(0); ST(0) = ST({i}); ST({i}) = _t; }}"
        if m in ("fcom", "fcomp", "fcompp", "fucom", "fucomp", "fucompp"):
            b = self.fsrc(ins, ops[0]) if ops else "ST(1)"
            pops = 2 if m.endswith("pp") else (1 if m.endswith("p") else 0)
            return f"fcom_set(ST(0), {b});" + " fpop();" * pops
        if m in ("fcomi", "fcomip", "fucomi", "fucomip", "fucompi", "fcompi"):
            i = self.streg(ins, ops[1]) if len(ops) > 1 else (self.streg(ins, ops[0]) if ops else 1)
            return f"fcomi_set(ST(0), ST({i}));" + (" fpop();" if m.endswith("p") or m.endswith("pi") else "")
        if m == "ftst": return "fcom_set(ST(0), 0.0);"
        if m == "fnstsw":
            if ops and ops[0].type == X.X86_OP_REG: return "SETLO16(cpu.eax, fsw_get());"
            return f"wr16({ea(ops[0])}, fsw_get());"
        if m == "fnstcw": return f"wr16({ea(ops[0])}, cpu.fcw);"
        if m == "fldcw": return f"cpu.fcw = rd16({ea(ops[0])});"
        if m in ("fnstenv", "fstenv"): return f"fenv_store({ea(ops[0])}); cpu.fcw |= 0x3F;"
        if m == "fldenv": return f"fenv_load({ea(ops[0])});"
        if m in ("fnsave", "fsave"): return f"{{ uint32_t _a = {ea(ops[0])}; fenv_store(_a); for (int _i = 0; _i < 8; _i++) wrf80(_a + 28 + 10 * _i, ST(_i)); cpu.top = 0; cpu.fsw = 0; cpu.fcw = 0x37F; }}"
        if m == "frstor": return f"{{ uint32_t _a = {ea(ops[0])}; fenv_load(_a); for (int _i = 0; _i < 8; _i++) ST(_i) = rdf80(_a + 28 + 10 * _i); }}"
        if m in ("fninit", "finit"): return "cpu.top = 0; cpu.fsw = 0; cpu.fcw = 0x27F;"
        if m in ("fnclex", "fclex"): return "cpu.fsw &= 0x7F00;"
        if m == "ffree": return ""
        if m == "fstp" or m == "fdecstp" or m == "fincstp": return None
        return None

    # ---------- une fonction ----------
    def function(self, entry):
        name, ranges = self.funcs[entry]
        insns = {}
        targets = set([entry])
        todo = [entry]
        while todo:
            a = todo.pop()
            while a not in insns and (a == entry or (a not in self.funcs and a not in self.thunks)) and (self.in_func(ranges, a) or self.text_lo <= a < self.text_hi):
                code = self.read(a, 16)
                ins = next(self.md.disasm(code, a, 1), None)
                if ins is None:
                    insns[a] = None
                    break
                insns[a] = ins
                m = ins.mnemonic
                if ins.group(capstone.CS_GRP_JUMP) or m in ("loop", "loope", "loopne", "jecxz", "jcxz"):
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xFFFFFFFF
                        targets.add(t)
                        if t == entry or (t not in self.funcs and t not in self.thunks): todo.append(t)
                    else:
                        for t in self.jumps.get(a, []):
                            targets.add(t)
                            if t == entry or (t not in self.funcs and t not in self.thunks): todo.append(t)
                    if m.replace("bnd ", "").replace("notrack ", "") == "jmp": break
                if ins.group(capstone.CS_GRP_RET) or m in ("int3", "hlt", "ud2") or m.endswith("ret"):
                    break
                a += ins.size
        lines = [f"/* {name} */", f"void f_{entry:08x}(void) {{"]
        for a in sorted(insns):
            ins = insns[a]
            if a in targets:
                lines.append(f"L_{a:08x}:;")
            if ins is None:
                lines.append(f"    UNIMPL(0x{a:x}, \"(non décodable)\"); return;")
                self.unimpl["(non décodable)"] += 1
                continue
            self.stats["instructions"] += 1
            try:
                c = self.control(ins, ranges, insns)
                if c is None:
                    c = self.tr(ins, None)
                    if c and ins.mnemonic in _STRIPPABLE and self.flags_dead(a, ins, insns):
                        c2 = _RE_FA.sub("", _RE_FL.sub("", c)); self.stats["drapeaux_morts"] += 1
                        c = c2 if c2.strip() else ""
                lines.append(f"    {c}" if c else f"    /* {a:08x} {ins.mnemonic} */")
            except (Unsupported, KeyError, IndexError, ValueError) as e:
                key = ins.mnemonic
                self.unimpl[key] += 1
                txt = (ins.mnemonic + " " + ins.op_str).replace('"', "'")
                lines.append(f"    UNIMPL(0x{a:x}, \"{txt}\");")
            # fin de bloc qui tombe hors fonction : appel terminal implicite
            nxt = a + ins.size
            if nxt not in insns and not (ins.group(capstone.CS_GRP_RET) or ins.mnemonic in ("jmp", "int3", "hlt", "ud2")):
                if nxt in self.funcs or nxt in self.thunks: lines.append(f"    f_{nxt:08x}(); return;")
                else: lines.append(f"    jmp_ind(0x{nxt:x}); return;")
        lines.append("}")
        return "\n".join(lines)

    def flags_dead(self, a, ins, insns):
        """vrai si aucun des drapeaux écrits par ins n'est lu sur le chemin séquentiel qui suit"""
        rem = _fwrites(ins)
        if not rem: return False
        n = a + ins.size
        for _ in range(64):
            j = insns.get(n)
            if j is None: return False                      # sortie de la fonction / non décodé : prudence
            m = j.mnemonic.replace("bnd ", "").replace("notrack ", "")
            if _freads(j) & rem: return False
            if m == "call": return True                     # les drapeaux ne survivent pas à un appel
            if j.group(capstone.CS_GRP_RET) or m.endswith("ret"): return True
            if j.group(capstone.CS_GRP_JUMP) or m in ("loop", "loope", "loopne", "jecxz"): return False
            rem &= ~_fwrites(j)
            if not rem: return True
            n += j.size
        return False

    def _has_backref(self, insns, entry):
        for ins in insns.values():
            if ins is not None and ins.group(capstone.CS_GRP_JUMP) and ins.operands[0].type == X.X86_OP_IMM and (ins.operands[0].imm & 0xFFFFFFFF) == entry:
                return True
        return False

    def goto_or_call(self, t, ranges, insns):
        if t in insns: return f"goto L_{t:08x};"
        if t in self.funcs or t in self.thunks: return f"{{ f_{t:08x}(); return; }}"
        return f"{{ jmp_ind(0x{t:x}); return; }}"

    def control(self, ins, ranges, insns):
        m = ins.mnemonic.replace("bnd ", "").replace("notrack ", "")
        a = ins.address
        nxt = a + ins.size
        ops = ins.operands
        if m == "call":
            ret = f"push32(0x{nxt:x}u); "
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xFFFFFFFF
                if t in self.funcs or t in self.thunks: return ret + f"f_{t:08x}();"
                return ret + f"call_ind(0x{t:x});"
            if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xFFFFFFFF) in self.imports:
                slot = op.mem.disp & 0xFFFFFFFF
                return ret + f"call_ind(rd32(0x{slot:x}u)); /* {self.imports[slot]} */"
            # la cible est lue AVANT d'empiler l'adresse de retour (call [esp+X])
            return f"{{ uint32_t _t = {self.get(ins, op, 32)}; {ret}call_ind(_t); }}"
        if m in ("ret", "retn"):
            n = ops[0].imm if ops else 0
            return f"cpu.esp += {4 + n}; return;"
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                return self.goto_or_call(op.imm & 0xFFFFFFFF, ranges, insns)
            if op.type == X.X86_OP_MEM and not op.mem.base and not op.mem.index and (op.mem.disp & 0xFFFFFFFF) in self.imports:
                slot = op.mem.disp & 0xFFFFFFFF
                return f"{{ jmp_ind(rd32(0x{slot:x}u)); return; }} /* {self.imports[slot]} */"
            tg = self.jumps.get(a, [])
            v = self.get(ins, op, 32)
            cases = " ".join(f"case 0x{t:x}u: goto L_{t:08x};" for t in sorted(set(tg)) if t in insns)
            return f"switch ({v}) {{ {cases} default: jmp_ind({v}); return; }}"
        if m.startswith("j") and m[1:] in CC:
            t = ops[0].imm & 0xFFFFFFFF
            return f"if ({CC[m[1:]]}) {self.goto_or_call(t, ranges, insns)}"
        if m in ("jecxz", "jcxz"):
            t = ops[0].imm & 0xFFFFFFFF
            return f"if (cpu.ecx == 0) {self.goto_or_call(t, ranges, insns)}"
        if m == "loop":
            t = ops[0].imm & 0xFFFFFFFF
            return f"if (--cpu.ecx) {self.goto_or_call(t, ranges, insns)}"
        if m in ("loope", "loopne"):
            t = ops[0].imm & 0xFFFFFFFF
            c = "cpu.zf" if m == "loope" else "!cpu.zf"
            return f"if (--cpu.ecx && {c}) {self.goto_or_call(t, ranges, insns)}"
        if m == "xlatb":
            return "SETLO8(cpu.eax, rd8(cpu.ebx + LO8(cpu.eax)));"
        return None


MASKS = {8: 0xFF, 16: 0xFFFF, 32: 0xFFFFFFFF, 64: 0xFFFFFFFFFFFFFFFF, 128: 0}


def uint(bits):
    return {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}[bits]


def sext(v, op, bits):
    # immédiat 8 bits étendu : capstone donne déjà la valeur signée étendue à la taille de l'opération
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe"); ap.add_argument("info"); ap.add_argument("--out", default="gen")
    ap.add_argument("--per-file", type=int, default=250)
    a = ap.parse_args()
    pe = pefile.PE(a.exe, fast_load=True)
    info = json.load(open(a.info, encoding="utf-8"))
    g = Gen(pe, info)
    os.makedirs(a.out, exist_ok=True)
    entries = sorted(set(g.funcs) | set(g.thunks))
    with open(os.path.join(a.out, "rc_funcs.h"), "w") as h:
        h.write(f"/* généré par x86recomp.py {VERSION} - ne pas éditer */\n#pragma once\n#include \"x86rt.h\"\n")
        for e in entries: h.write(f"void f_{e:08x}(void);\n")
    nfile = 0
    for i in range(0, len(entries), a.per_file):
        with open(os.path.join(a.out, f"rc_{nfile:03d}.c"), "w") as f:
            f.write(f"/* généré par x86recomp.py {VERSION} - ne pas éditer */\n#include \"rc_funcs.h\"\nvoid x86_cpuid(void); void x86_rdtsc(void);\n\n")
            for e in entries[i:i + a.per_file]:
                if e in g.thunks:
                    f.write(f"/* thunk {g.imports[g.thunks[e]]} */\nvoid f_{e:08x}(void) {{ jmp_ind(rd32(0x{g.thunks[e]:x}u)); }}\n\n")
                else:
                    f.write(g.function(e) + "\n\n")
        nfile += 1
    with open(os.path.join(a.out, "rc_table.c"), "w") as f:
        f.write("#include \"rc_funcs.h\"\nconst struct { uint32_t addr; void (*fn)(void); } rc_table[] = {\n")
        for e in entries: f.write(f"  {{0x{e:08x}u, f_{e:08x}}},\n")
        f.write(f"}};\nconst unsigned rc_table_n = {len(entries)};\n")
    with open(os.path.join(a.out, "rc_imports.c"), "w") as f:
        f.write("#include <stdint.h>\nconst struct { uint32_t slot; const char *name; } rc_imports[] = {\n")
        for s in sorted(g.imports): f.write(f"  {{0x{s:08x}u, \"{g.imports[s]}\"}},\n")
        f.write(f"}};\nconst unsigned rc_imports_n = {len(g.imports)};\n")
    tot = g.stats["instructions"]; un = sum(g.unimpl.values())
    with open(os.path.join(a.out, "rc_report.txt"), "w") as f:
        f.write(f"x86recomp {VERSION} : {len(entries)} fonctions, {tot} instructions, {un} non gérées ({100.0 * un / max(tot, 1):.2f} %)\n\n")
        for k, v in g.unimpl.most_common(): f.write(f"{v:7d}  {k}\n")
    print(f"{len(g.thunks)} thunks d'import, {g.n_extra} fonctions ajoutées (pointeurs dans les données)")
    print(f"{len(entries)} fonctions, {tot} instructions, {un} non gérées ({100.0 * un / max(tot, 1):.2f} %), {g.stats['drapeaux_morts']} calculs de drapeaux retirés, {nfile} fichiers -> {a.out}")


if __name__ == "__main__":
    main()
