#!/usr/bin/env python3
"""Static recompiler: ARM9 code (static module + overlays) -> C.

Usage:
  python -I tools/recomp/recomp.py --layout build/extracted/layout.json --out build/recomp

Every discovered function becomes a C function taking the CPU state. Direct
branches become gotos, BL becomes a C call, and anything that can't be
resolved statically goes through rt_call/rt_jump, which look the target up
in the generated dispatch table at run time.
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from armdecode import Image, disasm, COND_NAMES, AL, PC, LR, SP  # noqa: E402

COND_MACRO = ["C_EQ", "C_NE", "C_CS", "C_CC", "C_MI", "C_PL", "C_VS", "C_VC",
              "C_HI", "C_LS", "C_GE", "C_LT", "C_GT", "C_LE", None, None]
MAX_FUNC_INSTRS = 20000
RECOMP_FLAG_CALLER_FRAME = 1  # must match recomp_runtime.h
FILE_SPLIT_LINES = 60000


def key_name(addr, thumb):
    return f"{addr:08X}{'t' if thumb else 'a'}"


class Module:
    """One address space to recompile: the static ARM9 image, or one overlay on top of it."""

    def __init__(self, name, overlay_id, image, code_ranges):
        self.name = name
        self.overlay_id = overlay_id      # -1 for static
        self.image = image                # Image with everything visible to this module
        self.code_ranges = code_ranges    # [(start, end)] owned by this module
        self.functions = {}               # (addr, thumb) -> Function
        self.external_calls = set()       # (addr, thumb) referenced but owned by another module

    def owns(self, addr):
        return any(s <= addr < e for s, e in self.code_ranges)

    def prefix(self):
        return "s" if self.overlay_id < 0 else f"o{self.overlay_id:03d}"

    def fn_name(self, addr, thumb):
        return f"F_{self.prefix()}_{key_name(addr, thumb)}"


class Function:
    def __init__(self, addr, thumb):
        self.addr, self.thumb = addr, thumb
        self.blocks = set()   # (addr, thumb) of every branch target inside the function
        self.branch_targets = set()  # targets of plain B instructions kept inside the function
        self.instrs = {}      # (addr, thumb) -> Instr, in traversal order
        self.jump_tables = {} # instr addr -> [target (addr, thumb)]
        self.manual_calls = {} # bx addr -> (addr, thumb) where execution resumes
        self.veneers = {}     # bx addr -> (addr, thumb) loaded from a literal just before
        self.pop_returns = set()  # bx addrs that return through a register just popped off the stack
        self.bad = None       # reason when the function can't be translated


# --------------------------------------------------------------------------- analysis

def find_switch_bound(fn, ins, reg):
    """Looks back a few instructions for `cmp reg, #N` guarding a jump table."""
    step = 2 if ins.thumb else 4
    for back in range(1, 8):
        prev = fn.instrs.get((ins.addr - back * step, ins.thumb))
        if prev is None:
            return None
        if prev.op == "cmp" and prev.rn == reg and prev.op2_imm:
            return prev.imm
        if prev.rd == reg:
            return None
    return None


def resolve_jump_table(mod, fn, ins):
    """Recognises the compiler's switch patterns. Returns [(addr, thumb)] or None."""
    img = mod.image
    # ARM: cmp rX, #N ; addls pc, pc, rX, lsl #2 ; b default ; b case0 ; ...
    if (not ins.thumb and ins.op == "add" and ins.rd == PC and ins.rn == PC and not ins.op2_imm
            and not ins.shift_reg and ins.shift == 0 and ins.shift_imm == 2):
        n = find_switch_bound(fn, ins, ins.rm)
        if n is None or n > 1024:
            return None
        return [(ins.pc_read + 4 * k, False) for k in range(n + 1)]
    # ARM: cmp rX, #N ; ldrls pc, [pc, rX, lsl #2] ; b default ; .word case0 ...
    if (not ins.thumb and ins.op == "ldr" and ins.rd == PC and ins.rn == PC and not ins.op2_imm
            and ins.shift == 0 and ins.shift_imm == 2 and ins.pre and ins.up and not ins.writeback):
        n = find_switch_bound(fn, ins, ins.rm)
        if n is None or n > 1024:
            return None
        out = []
        for k in range(n + 1):
            t = img.u32(ins.pc_read + 4 * k)
            if t is None:
                return None
            out.append((t & ~1, bool(t & 1)))
        return out
    # Thumb: add r0, rI, rI ; add r0, pc ; ldrh r0, [r0, #k] ; lsl r0, #16 ; asr r0, #16 ; add pc, r0
    if ins.thumb and ins.op == "add" and ins.rd == PC and ins.rn == PC and not ins.op2_imm:
        seq = [fn.instrs.get((ins.addr - 2 * k, True)) for k in range(1, 6)]
        if None in seq:
            return None
        asr, lsl, ldrh, addpc, dbl = seq
        r = ins.rm
        if not (asr.op == "mov" and asr.shift == 2 and asr.shift_imm == 16 and asr.rd == r
                and lsl.op == "mov" and lsl.shift == 0 and lsl.shift_imm == 16
                and ldrh.op == "ldr" and ldrh.width == "h" and ldrh.op2_imm
                and addpc.op == "add" and addpc.rm == PC and dbl.op == "add" and dbl.rn == dbl.rm):
            return None
        table = addpc.addr + 4 + ldrh.imm
        n = find_switch_bound(fn, dbl, dbl.rn)
        out = []
        limit = (n + 1) if n is not None else 1024
        first_target = None
        for k in range(limit):
            if n is None and first_target is not None and table + 2 * k >= first_target:
                break  # no bound found: the table ends where the code it points at begins
            h = img.u16(table + 2 * k)
            if h is None:
                return None
            # A Thumb `add pc, rX` drops bit 0 of the result.
            t = (ins.addr + 4 + (h - 0x10000 if h & 0x8000 else h)) & 0xFFFFFFFE
            first_target = t if first_target is None else min(first_target, t)
            out.append((t, True))
        return out
    return None


def writes_reg(ins, r):
    if ins.op in ("ldm",) and ins.load:
        return bool(ins.regs >> r & 1) or (ins.writeback and ins.rn == r)
    if ins.op in ("ldr", "str") and ins.writeback and ins.rn == r:
        return True
    if ins.op in ("umull", "umlal", "smull", "smlal_l", "smlal") and ins.ra == r:
        return True
    if ins.op == "ldr" and ins.width == "d" and ins.rd is not None and ins.rd + 1 == r:
        return True
    if ins.op in ("str", "stm", "cmp", "cmn", "tst", "teq", "mcr", "msr", "swi", "nop"):
        return False
    return ins.rd == r


def find_def(fn, ins, r, lookback=8):
    """The straight-line instruction that last wrote register r before `ins`, or None.

    Stops at anything that could change r invisibly (calls, branches, other states)."""
    step = 2 if ins.thumb else 4
    for k in range(1, lookback + 1):
        prev = fn.instrs.get((ins.addr - k * step, ins.thumb))
        if prev is None or prev.size != step or prev.cond != ins.cond:
            return None
        if writes_reg(prev, r):
            return prev
        if prev.op in ("b", "bx", "swi") or ends_path(prev):
            return None
    return None


def literal_value(img, ldr):
    """Value of `ldr rX, =const` (PC-relative literal), or None."""
    if not (ldr.op == "ldr" and ldr.rn == PC and ldr.op2_imm and ldr.up and not ldr.writeback
            and ldr.width == "w"):
        return None
    base = (ldr.addr + 4) & ~3 if ldr.thumb else ldr.pc_read
    return img.u32(base + ldr.imm)


def manual_call_return(img, fn, ins):
    """LR set to a nearby address (`mov lr, pc` / `ldr lr, =ret` / `add lr, pc, #n`)
    before `bx rN` makes it a call. Returns the (addr, thumb) execution resumes at, or None."""
    if ins.op != "bx" or ins.link or ins.rm in (LR, PC):
        return None
    d = find_def(fn, ins, LR)
    if d is None:
        return None
    if d.op == "mov" and d.rm == PC and not d.op2_imm and not d.shift_imm:
        return (d.pc_read, d.thumb)
    if d.op == "add" and d.rn == PC and d.op2_imm:
        base = (d.addr + 4) & ~3 if d.thumb else d.pc_read
        return (base + d.imm, d.thumb)
    v = literal_value(img, d)
    if v is not None:
        return (v & ~1, bool(v & 1))
    return None


def popped_return(fn, ins):
    """`pop {..., rX} ; [a few instructions] ; bx rX`: Thumb code returns this way
    (e.g. `pop {r3}; movs r0, #1; bx r3`)."""
    if ins.op != "bx" or ins.link or ins.rm in (PC, LR):
        return False
    d = find_def(fn, ins, ins.rm)
    if d is None:
        return False
    if d.op == "ldm" and d.rn == SP and d.writeback and d.up and not d.pre and d.regs >> ins.rm & 1:
        return True
    return (d.op == "ldr" and d.rd == ins.rm and d.rn == SP and not d.pre and d.up
            and d.op2_imm and d.width == "w")


def literal_bx_target(img, fn, ins):
    """`ldr rN, =func ; [argument setup] ; bx rN` (veneer / tail call through a literal).
    Returns (addr, thumb) or None."""
    if ins.op != "bx" or ins.link or ins.rm in (PC, LR):
        return None
    d = find_def(fn, ins, ins.rm)
    v = None if d is None else literal_value(img, d)
    return None if v is None else (v & ~1, bool(v & 1))


def is_return(ins):
    if ins.op == "bx" and not ins.link and ins.rm == LR:
        return True
    if ins.op == "mov" and ins.rd == PC and ins.rm == LR and not ins.op2_imm and not ins.shift_imm and not ins.set_flags:
        return True
    if ins.op == "ldm" and ins.regs >> PC & 1 and ins.rn == SP and not ins.user:
        return True
    if (ins.op == "ldr" and ins.rd == PC and ins.rn == SP and not ins.pre and ins.op2_imm
            and ins.up and ins.width == "w"):
        return True
    return False


def writes_pc(ins):
    if ins.op in ("ldm",) and ins.regs >> PC & 1:
        return True
    if ins.op == "ldr" and ins.rd == PC:
        return True
    if ins.rd == PC and ins.op in ("and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "orr", "mov", "bic", "mvn"):
        return True
    return False


def ends_path(ins):
    """True when execution never continues at the next instruction."""
    if ins.op in ("undef", "bkpt"):
        return True
    if ins.cond != AL:
        return False
    if ins.op == "b":
        return not ins.link
    if ins.op == "bx":
        return not ins.link
    if ins.op == "swi":
        return (ins.imm & 0xFF if ins.thumb else (ins.imm >> 16) & 0xFF) == 0
    return writes_pc(ins)


def explore(mod, fn, known_entries, new_calls):
    """Walks the function's control flow. Collects call targets into new_calls."""
    work = [(fn.addr, fn.thumb)]
    fn.blocks.add((fn.addr, fn.thumb))
    seen = set()
    while work:
        addr, thumb = work.pop()
        while (addr, thumb) not in seen:
            if len(fn.instrs) > MAX_FUNC_INSTRS:
                fn.bad = "too large"
                return
            if not mod.image.contains(addr):
                # Branch into code this module can't see (another overlay's
                # region): the emitter hands that path to the dispatcher.
                break
            seen.add((addr, thumb))
            ins = mod.image.decode(addr, thumb)
            if ins is None:
                fn.bad = f"undecodable at {addr:08X}"
                return
            fn.instrs[(addr, thumb)] = ins
            nxt = addr + ins.size
            cond = ins.cond != AL

            if ins.op == "undef":
                break
            if ins.op == "b":
                tgt_thumb = thumb ^ ins.exchange
                tgt = (ins.target, tgt_thumb)
                if ins.link:
                    new_calls.add(tgt)
                elif tgt in known_entries and tgt != (fn.addr, fn.thumb):
                    pass  # tail call
                else:
                    fn.branch_targets.add(tgt)
                    if tgt not in fn.blocks:
                        fn.blocks.add(tgt)
                        work.append(tgt)
                    if not cond:
                        break
                if not ins.link and not cond:
                    break
                if cond:
                    fn.blocks.add((nxt, thumb))
            elif ins.op == "bx":
                if thumb and ins.rm == PC and not ins.link:
                    # `bx pc` veneer: continue in ARM state at the next word.
                    tgt = ((addr + 4) & ~3, False)
                    fn.blocks.add(tgt)
                    work.append(tgt)
                    break
                if popped_return(fn, ins):
                    fn.pop_returns.add(addr)
                    if not cond:
                        break
                    fn.blocks.add((nxt, thumb))
                    addr = nxt
                    continue
                veneer = literal_bx_target(mod.image, fn, ins)
                if veneer:
                    fn.veneers[addr] = veneer
                    new_calls.add(veneer)
                resume = manual_call_return(mod.image, fn, ins)
                if resume and mod.image.contains(resume[0]):
                    fn.manual_calls[addr] = resume
                    if resume not in fn.blocks:
                        fn.blocks.add(resume)
                        work.append(resume)
                    if cond:
                        fn.blocks.add((nxt, thumb))
                    else:
                        break
                elif not ins.link and not cond:
                    break
            elif writes_pc(ins):
                table = resolve_jump_table(mod, fn, ins)
                if table:
                    fn.jump_tables[addr] = table
                    for t in table:
                        if t not in fn.blocks:
                            fn.blocks.add(t)
                            work.append(t)
                if not cond:
                    break
                fn.blocks.add((nxt, thumb))
            elif ins.op == "swi" and not cond and (ins.imm & 0xFF if thumb else (ins.imm >> 16) & 0xFF) == 0:
                break  # SoftReset never returns
            addr = nxt


def scan_pointers(mod, entries):
    """Adds function entries referenced by data words (vtables, callbacks)."""
    found = set()
    for start, data, _ in mod.image.segs:
        if not mod.owns(start):
            continue
        for off in range(0, len(data) - 3, 4):
            v = int.from_bytes(data[off:off + 4], "little")
            if not mod.owns(v & ~1):
                continue
            thumb = bool(v & 1)
            addr = v & ~1
            if (addr, thumb) in entries:
                continue
            if plausible_code(mod, addr, thumb):
                found.add((addr, thumb))
    return found


def plausible_code(mod, addr, thumb, limit=24):
    """Straight-line decode from addr reaches a path end without hitting garbage.

    A false positive only adds a harmless extra entry point; data words that
    merely look like code addresses get rejected because they don't decode."""
    if addr % (2 if thumb else 4):
        return False
    first = mod.image.decode(addr, thumb)
    if first is None or first.op in ("undef", "bkpt") or (not thumb and first.cond == 15):
        return False
    a = addr
    for _ in range(limit):
        ins = mod.image.decode(a, thumb)
        if ins is None or ins.op in ("undef", "bkpt") or not mod.owns(a):
            return False
        if ends_path(ins) or is_return(ins):
            return True
        a += ins.size
    # Long straight-line code without a terminator: accept only with a real prologue.
    return first.op == "stm" and first.rn == SP and bool(first.regs >> LR & 1)


def analyse(mod, seeds, weak_seeds=frozenset()):
    """Discovers the module's functions.

    Strong entries (entry point, sinit tables, BL targets, veneers) are certain
    function starts: a plain B to one is a tail call. Weak entries (data pointers,
    run-time seeds) may point into the middle of a function (e.g. a loop head), so
    they get their own C function for the dispatcher but never split other
    functions' control flow (that would turn a loop's back-edge into one host call
    per iteration)."""
    strong = mod.strong = getattr(mod, "strong", set()) | set(s for s in seeds if mod.owns(s[0]))
    mod.weak_seeds = getattr(mod, "weak_seeds", set()) | set(
        s for s in weak_seeds if mod.owns(s[0]) and plausible_code(mod, s[0], s[1]))
    weak = (scan_pointers(mod, strong) | mod.weak_seeds) - strong
    pending = list(strong | weak)
    mod.functions = {}
    while pending:
        done = set()
        while pending:
            key = pending.pop()
            if key in done:
                continue
            done.add(key)
            fn = Function(*key)
            calls = set()
            explore(mod, fn, strong, calls)
            mod.functions[key] = fn
            for call in calls:
                if mod.owns(call[0]):
                    if call not in strong:
                        strong.add(call)
                        pending.append(call)
                else:
                    mod.external_calls.add(call)
        # A function explored before one of its branch targets became a strong
        # entry kept that code inline; re-explore it so the branch becomes a tail call.
        stale = [k for k, f in mod.functions.items() if any(t in strong and t != k for t in f.branch_targets)]
        for k in stale:
            pending.append(k)
            del mod.functions[k]

    # Drop weak entries that are really data, code read in the wrong state (a
    # Thumb pointer into the middle of ARM code decodes as half instructions),
    # or a function's tail reached through a data word (pops a PC it never pushed).
    bytes_in_state = {False: set(), True: set()}
    for key, fn in mod.functions.items():
        if key in strong:
            for (a, t), ins in fn.instrs.items():
                bytes_in_state[t].update(range(a, a + ins.size))
    for key in [k for k in mod.functions if k not in strong]:
        fn = mod.functions[key]
        wrong_state = key[0] in bytes_in_state[not key[1]]
        if wrong_state or fn.bad or pops_unpushed_pc(fn) or any(i.op == "undef" for i in fn.instrs.values()):
            del mod.functions[key]


def pops_unpushed_pc(fn):
    """Returns through a PC popped off the stack but never pushes LR itself.

    Real code does this when a caller pushed LR and then branched here (e.g. the
    SDK's thread scheduler, entered with B from the IRQ handler)."""
    # Walk from the entry; a push of LR ends that path, a pop-return before it proves it.
    seen = set()
    work = [(fn.addr, fn.thumb)]
    while work:
        key = work.pop()
        if key in seen or key not in fn.instrs:
            continue
        seen.add(key)
        ins = fn.instrs[key]
        if ins.op == "stm" and ins.rn == SP and ins.regs >> LR & 1:
            continue
        if (is_return(ins) and ins.op in ("ldm", "ldr")) or key[0] in fn.pop_returns:
            return True
        if ins.op == "b" and not ins.link and ins.target is not None:
            work.append((ins.target, key[1] ^ ins.exchange))
        for t in fn.jump_tables.get(key[0], []):
            work.append(t)
        if key[0] in fn.manual_calls:
            work.append(fn.manual_calls[key[0]])
        if not ends_path(ins):
            work.append((key[0] + ins.size, key[1]))
    return False


# --------------------------------------------------------------------------- C emission

class Emitter:
    def __init__(self, mod, all_modules, stats):
        self.mod = mod
        self.all = all_modules
        self.stats = stats
        self.out = []

    def R(self, ins, r):
        if r == PC:
            return f"0x{ins.pc_read:08X}u"
        return f"c->r[{r}]"

    def label(self, addr, thumb):
        return f"L_{key_name(addr, thumb)}"

    def callee(self, addr, thumb):
        """C expression calling a known target, or None when it needs the dispatcher."""
        key = (addr, thumb)
        if key in self.mod.functions and not self.mod.functions[key].bad:
            return self.mod.fn_name(addr, thumb)
        static = self.all[0]
        if self.mod is not static and key in static.functions and not static.functions[key].bad:
            return static.fn_name(addr, thumb)
        return None

    def emit_call(self, addr, thumb, w):
        name = self.callee(addr, thumb)
        if name:
            w(f"{name}(c);")
        else:
            w(f"rt_call(c, 0x{addr | (1 if thumb else 0):08X}u);")
            self.stats["dispatch_calls"] += 1

    def op2(self, ins, w):
        """Emits the shifter operand into `op2`, carry-out into `sc` (pre-set to C)."""
        if ins.op2_imm:
            w(f"u32 op2 = 0x{ins.imm:X}u;")
            if ins.shift_imm:  # rotated immediate: carry = bit 31
                w(f"u32 sc = {ins.imm >> 31}u;")
            else:
                w("u32 sc = c->c;")
            return
        v = self.R(ins, ins.rm)
        if ins.shift_reg:
            fn = ["sh_lsl", "sh_lsr", "sh_asr", "sh_ror"][ins.shift]
            w(f"u32 sc = c->c; u32 op2 = {fn}({v}, c->r[{ins.rs}], &sc);")
            return
        n, s = ins.shift_imm, ins.shift
        if s == 0:
            if n == 0:
                w(f"u32 op2 = {v}; u32 sc = c->c;")
            else:
                w(f"u32 op2 = {v} << {n}; u32 sc = ({v} >> {32 - n}) & 1;")
        elif s == 1:
            if n == 0:
                w(f"u32 op2 = 0; u32 sc = {v} >> 31;")
            else:
                w(f"u32 op2 = {v} >> {n}; u32 sc = ({v} >> {n - 1}) & 1;")
        elif s == 2:
            if n == 0:
                w(f"u32 op2 = (u32)((s32){v} >> 31); u32 sc = {v} >> 31;")
            else:
                w(f"u32 op2 = (u32)((s32){v} >> {n}); u32 sc = ({v} >> {n - 1}) & 1;")
        else:
            if n == 0:
                w(f"u32 op2 = (c->c << 31) | ({v} >> 1); u32 sc = {v} & 1;")
            else:
                w(f"u32 op2 = ror32({v}, {n}); u32 sc = ({v} >> {n - 1}) & 1;")

    def addr_offset(self, ins, w):
        """Load/store offset into `off`."""
        if ins.op2_imm:
            w(f"u32 off = 0x{ins.imm:X}u;")
        elif ins.width in ("w", "b"):
            self.op2(ins, w)
            w("u32 off = op2; (void)sc;")
        else:
            w(f"u32 off = {self.R(ins, ins.rm)};")

    def emit_dp(self, ins, w, fn):
        op = ins.op
        S = ins.set_flags
        self.op2(ins, w)
        if ins.rn is not None:
            # Thumb `add rd, pc, #imm` reads PC word-aligned; the hi-register form doesn't.
            if ins.rn == PC and ins.thumb and ins.op2_imm:
                w(f"u32 a = 0x{(ins.addr + 4) & ~3:08X}u;")
            else:
                w(f"u32 a = {self.R(ins, ins.rn)};")
        logical = op in ("and", "eor", "tst", "teq", "orr", "mov", "bic", "mvn")
        expr = {
            "and": "a & op2", "eor": "a ^ op2", "tst": "a & op2", "teq": "a ^ op2",
            "orr": "a | op2", "mov": "op2", "bic": "a & ~op2", "mvn": "~op2",
            "add": "a + op2", "cmn": "a + op2", "sub": "a - op2", "cmp": "a - op2",
            "rsb": "op2 - a", "adc": "a + op2 + c->c", "sbc": "a - op2 - !c->c", "rsc": "op2 - a - !c->c",
        }[op]
        w(f"u32 res = {expr};")
        if S:
            w("NZ(res);")
            if logical:
                w("c->c = sc;")
            elif op in ("add", "cmn"):
                w("c->c = res < a; c->v = (~(a ^ op2) & (a ^ res)) >> 31;")
            elif op in ("sub", "cmp"):
                w("c->c = a >= op2; c->v = ((a ^ op2) & (a ^ res)) >> 31;")
            elif op == "rsb":
                w("c->c = op2 >= a; c->v = ((op2 ^ a) & (op2 ^ res)) >> 31;")
            elif op == "adc":
                w("c->c = ((u64)a + op2 + c->c) >> 32; c->v = (~(a ^ op2) & (a ^ res)) >> 31;")
            elif op == "sbc":
                w("c->c = (u64)a >= (u64)op2 + !c->c; c->v = ((a ^ op2) & (a ^ res)) >> 31;")
            elif op == "rsc":
                w("c->c = (u64)op2 >= (u64)a + !c->c; c->v = ((op2 ^ a) & (op2 ^ res)) >> 31;")
        else:
            w("(void)sc;")
        if ins.rd is None:
            return
        if ins.rd != PC:
            w(f"c->r[{ins.rd}] = res;")
            return
        # Writes to PC are control flow.
        if S and not ins.thumb:
            w(f"rt_exception_return(c, 0x{ins.addr:08X}u); return;")
            self.stats["exception_returns"] += 1
        elif is_return(ins):
            w("return;")
        elif ins.addr in fn.jump_tables:
            self.emit_table_jump(ins, w, fn, "(res & ~1u)" if ins.thumb else "res")
        else:
            w(f"rt_jump(c, res{' | 1' if ins.thumb else ''}, 0x{ins.addr:08X}u); return;")
            self.stats["dispatch_jumps"] += 1

    def emit_table_jump(self, ins, w, fn, value):
        w(f"switch ({value}) {{")
        for t in sorted(set(fn.jump_tables[ins.addr])):
            v = t[0] | (1 if t[1] else 0) if ins.op == "ldr" else t[0]
            w(f"case 0x{v:08X}u: goto {self.label(*t)};")
        w(f"default: rt_jump(c, {value}, 0x{ins.addr:08X}u); return;")
        w("}")

    def emit_mem(self, ins, w, fn):
        n = ins.rn
        if n == PC and ins.thumb:
            w(f"u32 base = 0x{(ins.addr + 4) & ~3:08X}u;")
        else:
            w(f"u32 base = {self.R(ins, n)};")
        self.addr_offset(ins, w)
        sign = "+" if ins.up else "-"
        w(f"u32 ea = {'base ' + sign + ' off' if ins.pre else 'base'};")
        wb = ins.writeback and n != PC
        if ins.load:
            rd_fn = {"w": "rd32_rot(c, ea)", "b": "rt_read8(c, ea)", "h": "rt_read16(c, ea)",
                     "sb": "(u32)(s32)(s8)rt_read8(c, ea)", "sh": "(u32)(s32)(s16)rt_read16(c, ea)"}
            if ins.width == "d":
                w("u32 v0 = rt_read32(c, ea); u32 v1 = rt_read32(c, ea + 4);")
            else:
                w(f"u32 val = {rd_fn[ins.width]};")
            if wb:
                w(f"c->r[{n}] = base {sign} off;")
            if ins.width == "d":
                w(f"c->r[{ins.rd}] = v0; c->r[{ins.rd + 1}] = v1;")
                return
            if ins.rd != PC:
                w(f"c->r[{ins.rd}] = val;")
                return
            if is_return(ins):
                w("RECOMP_RETURN_CHECK(c, val, entry_lr); return;")
            elif ins.addr in fn.jump_tables:
                self.emit_table_jump(ins, w, fn, "val")
            else:
                w(f"rt_jump(c, val, 0x{ins.addr:08X}u); return;")
                self.stats["dispatch_jumps"] += 1
            return
        # store
        if ins.width == "d":
            w(f"rt_write32(c, ea, {self.R(ins, ins.rd)}); rt_write32(c, ea + 4, {self.R(ins, ins.rd + 1)});")
        else:
            val = f"0x{ins.addr + 12:08X}u" if ins.rd == PC else f"c->r[{ins.rd}]"
            wr = {"w": "rt_write32(c, ea & ~3u, ", "b": "rt_write8(c, ea, ", "h": "rt_write16(c, ea, "}[ins.width]
            w(f"{wr}{val});")
        if wb:
            w(f"c->r[{n}] = base {sign} off;")

    def emit_block_transfer(self, ins, w, fn):
        regs = ins.reg_list()
        cnt = len(regs)
        n = ins.rn
        w(f"u32 base = c->r[{n}];")
        if ins.up:
            start = "base + 4" if ins.pre else "base"
            new_base = f"base + {4 * cnt}"
        else:
            start = f"base - {4 * cnt}" if ins.pre else f"base - {4 * cnt - 4}"
            new_base = f"base - {4 * cnt}"
        w(f"u32 ea = {start};")
        if ins.load:
            for r in regs:
                if r == PC:
                    w("u32 newpc = rt_read32(c, ea); ea += 4;")
                else:
                    w(f"c->r[{r}] = rt_read32(c, ea); ea += 4;")
            if ins.writeback and not (ins.regs >> n & 1):
                w(f"c->r[{n}] = {new_base};")
            if ins.regs >> PC & 1:
                if ins.user:
                    w(f"rt_exception_return(c, 0x{ins.addr:08X}u); return;")
                    self.stats["exception_returns"] += 1
                elif is_return(ins):
                    w("RECOMP_RETURN_CHECK(c, newpc, entry_lr); (void)newpc; return;")
                else:
                    w(f"rt_jump(c, newpc, 0x{ins.addr:08X}u); return;")
                    self.stats["dispatch_jumps"] += 1
            return
        for r in regs:
            v = f"0x{ins.addr + 12:08X}u" if r == PC else ("base" if r == n else f"c->r[{r}]")
            w(f"rt_write32(c, ea, {v}); ea += 4;")
        if ins.writeback:
            w(f"c->r[{n}] = {new_base};")

    def emit_mul(self, ins, w):
        op = ins.op
        rm, rs = f"c->r[{ins.rm}]", f"c->r[{ins.rs}]"
        if op in ("mul", "mla"):
            acc = f" + c->r[{ins.ra}]" if op == "mla" else ""
            w(f"u32 res = {rm} * {rs}{acc};")
            if ins.set_flags:
                w("NZ(res);")
            w(f"c->r[{ins.rd}] = res;")
            return
        if op in ("umull", "umlal", "smull", "smlal_l"):
            signed = op.startswith("s")
            cast = "(s64)(s32)" if signed else "(u64)"
            w(f"u64 res = (u64)({cast}{rm} * {cast}{rs});")
            if op in ("umlal", "smlal_l"):
                w(f"res += ((u64)c->r[{ins.rd}] << 32) | c->r[{ins.ra}];")
            w(f"c->r[{ins.ra}] = (u32)res; c->r[{ins.rd}] = (u32)(res >> 32);")
            if ins.set_flags:
                w("c->n = (u32)(res >> 63); c->z = res == 0;")
            return
        x, y = ins.extra
        hx = f"(s32)(s16)({rm} >> {16 * x})"
        hy = f"(s32)(s16)({rs} >> {16 * y})"
        if op == "smul":
            w(f"c->r[{ins.rd}] = (u32)({hx} * {hy});")
        elif op in ("smla", "smlaw"):
            # The accumulate wraps; Q only records that it overflowed.
            prod = f"(s64)({hx} * {hy})" if op == "smla" else f"(s64)(s32)(((s64)(s32){rm} * {hy}) >> 16)"
            w(f"s64 acc = {prod} + (s32)c->r[{ins.ra}]; if (acc != (s32)acc) c->q = 1;")
            w(f"c->r[{ins.rd}] = (u32)acc;")
        elif op == "smulw":
            w(f"c->r[{ins.rd}] = (u32)(((s64)(s32){rm} * {hy}) >> 16);")
        elif op == "smlal":
            w(f"s64 acc = (s64)(((u64)c->r[{ins.rd}] << 32) | c->r[{ins.ra}]) + (s64)({hx} * {hy});")
            w(f"c->r[{ins.ra}] = (u32)acc; c->r[{ins.rd}] = (u32)((u64)acc >> 32);")

    def emit_instr(self, ins, fn, w):
        op = ins.op
        self.stats["ops"][op] += 1
        if op in ("and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn",
                  "orr", "mov", "bic", "mvn"):
            self.emit_dp(ins, w, fn)
        elif op in ("ldr", "str"):
            if op == "ldr" and ins.rn == PC and ins.op2_imm and not ins.writeback and ins.rd != PC and ins.width == "w":
                base = (ins.addr + 4) & ~3 if ins.thumb else ins.addr + 8
                ea = base + ins.imm if ins.up else base - ins.imm
                val = self.mod.image.u32(ea)
                if val is not None and ea % 4 == 0:
                    w(f"c->r[{ins.rd}] = 0x{val:08X}u; /* literal @{ea:08X} */")
                    return
            self.emit_mem(ins, w, fn)
        elif op in ("ldm", "stm"):
            self.emit_block_transfer(ins, w, fn)
        elif op == "b":
            tgt_thumb = ins.thumb ^ ins.exchange
            if ins.link:
                ret = ins.addr + ins.size
                w(f"c->r[14] = 0x{ret | (1 if ins.thumb else 0):08X}u;")
                self.emit_call(ins.target, tgt_thumb, w)
            elif (ins.target, tgt_thumb) in fn.blocks:
                w(f"goto {self.label(ins.target, tgt_thumb)};")
            else:
                self.emit_call(ins.target, tgt_thumb, w)
                w("return;")
                self.stats["tail_calls"] += 1
        elif op == "bx":
            if ins.thumb and ins.rm == PC and not ins.link:
                w(f"goto {self.label((ins.addr + 4) & ~3, False)};")
            elif ins.link:
                w(f"u32 t = c->r[{ins.rm}]; c->r[14] = 0x{(ins.addr + ins.size) | (1 if ins.thumb else 0):08X}u; rt_call(c, t);")
                self.stats["dispatch_calls"] += 1
            elif ins.addr in fn.manual_calls:
                if ins.addr in fn.veneers:
                    self.emit_call(*fn.veneers[ins.addr], w)
                else:
                    w(f"rt_call(c, c->r[{ins.rm}]);")
                    self.stats["dispatch_calls"] += 1
                w(f"goto {self.label(*fn.manual_calls[ins.addr])};")
            elif ins.addr in fn.veneers:
                self.emit_call(*fn.veneers[ins.addr], w)
                w("return;")
                self.stats["tail_calls"] += 1
            elif ins.addr in fn.pop_returns:
                w(f"RECOMP_RETURN_CHECK(c, c->r[{ins.rm}], entry_lr); return;")
            elif ins.rm == LR:
                w("return;")
            else:
                w(f"rt_jump(c, c->r[{ins.rm}], 0x{ins.addr:08X}u); return;")
                self.stats["dispatch_jumps"] += 1
        elif op in ("mul", "mla", "umull", "umlal", "smull", "smlal_l", "smul", "smla", "smulw", "smlaw", "smlal"):
            self.emit_mul(ins, w)
        elif op == "clz":
            w(f"c->r[{ins.rd}] = clz32(c->r[{ins.rm}]);")
        elif op in ("qadd", "qsub", "qdadd", "qdsub"):
            a, b = f"(s64)(s32)c->r[{ins.rm}]", f"(s64)(s32)c->r[{ins.rn}]"
            if op.startswith("qd"):
                w(f"s64 d = (s64)(s32)sat_add(c, 2 * {b});")
                b = "d"
            sign = "+" if op.endswith("add") else "-"
            w(f"c->r[{ins.rd}] = sat_add(c, {a} {sign} {b});")
        elif op == "swp":
            if ins.width == "b":
                w(f"u32 t = rt_read8(c, c->r[{ins.rn}]); rt_write8(c, c->r[{ins.rn}], c->r[{ins.rm}]); c->r[{ins.rd}] = t;")
            else:
                w(f"u32 t = rd32_rot(c, c->r[{ins.rn}]); rt_write32(c, c->r[{ins.rn}] & ~3u, c->r[{ins.rm}]); c->r[{ins.rd}] = t;")
        elif op == "mrs":
            if ins.extra:
                w(f"c->r[{ins.rd}] = rt_get_psr(c, 1);")
            else:
                # The CPU state is known statically at every instruction.
                t = " | 0x20u" if ins.thumb else ""
                w(f"c->r[{ins.rd}] = (rt_get_psr(c, 0) & ~0x20u){t};")
        elif op == "msr":
            mask = ins.rs if ins.op2_imm else ins.imm
            val = f"0x{ins.imm:X}u" if ins.op2_imm else f"c->r[{ins.rm}]"
            w(f"rt_set_psr(c, {val}, 0x{mask:X}u, {int(ins.extra)});")
        elif op in ("mrc", "mcr"):
            cp, op1, crn, crm, op2 = ins.extra
            ident = (cp << 16) | (op1 << 12) | (crn << 8) | (crm << 4) | op2
            if op == "mrc":
                if ins.rd == PC:
                    w(f"u32 v = rt_mrc(c, 0x{ident:X}u); c->n = v >> 31 & 1; c->z = v >> 30 & 1; c->c = v >> 29 & 1; c->v = v >> 28 & 1;")
                else:
                    w(f"c->r[{ins.rd}] = rt_mrc(c, 0x{ident:X}u);")
            else:
                w(f"rt_mcr(c, 0x{ident:X}u, {self.R(ins, ins.rd)});")
        elif op == "swi":
            num = ins.imm & 0xFF if ins.thumb else (ins.imm >> 16) & 0xFF
            w(f"rt_swi(c, 0x{num:X}u, 0x{ins.addr:08X}u);")
        elif op == "nop":
            pass
        else:
            w(f"rt_unimpl(c, 0x{ins.addr:08X}u, 0x{ins.raw:08X}u); return;")
            self.stats["unimpl"][f"{op} {ins.extra or ''}".strip()] += 1

    def emit_function(self, fn):
        out = self.out
        name = self.mod.fn_name(fn.addr, fn.thumb)
        out.append(f"void {name}(Cpu* c)\n{{")
        # LR on entry: what a return through a popped value should give back (checked in tests).
        out.append("    u32 entry_lr = c->r[14]; (void)entry_lr;")
        out.append(f"    goto {self.label(fn.addr, fn.thumb)};")
        # Emit instructions in address order; fallthrough between consecutive
        # instructions is implicit, everything else is an explicit goto.
        keys = sorted(fn.instrs, key=lambda k: (k[1], k[0]))
        prev_end = None
        for key in keys:
            ins = fn.instrs[key]
            if prev_end is not None and prev_end != key:
                # gap: previous instruction falls through to something not emitted next
                out.append(f"    goto {self.label(*prev_end)};")
            if key in fn.blocks:
                # Block entries are where loops close: the runtime hooks in here
                # (interrupt polling, test budgets).
                out.append(f"{self.label(*key)}:; RECOMP_BLOCK_HOOK(c);")
            elif prev_end != key:
                out.append(f"{self.label(*key)}:;")
            body = []
            self.emit_instr(ins, fn, body.append)
            text = disasm(ins).replace("*/", "* /")
            cmacro = COND_MACRO[ins.cond]
            if cmacro and body:
                out.append(f"    if ({cmacro}) {{ {' '.join(body)} }} /* {text} */")
            elif body:
                out.append(f"    {{ {' '.join(body)} }} /* {text} */")
            else:
                out.append(f"    /* {text} */")
            self.stats["instructions"] += 1
            prev_end = None if ends_path(ins) else (key[0] + ins.size, key[1])
        if prev_end is not None:
            out.append(f"    goto {self.label(*prev_end)};")
        out.append("}")
        out.append("")

    def emit_module(self, fns):
        for fn in fns:
            start = len(self.out)
            self.emit_function(fn)
            # Gotos to code the walk never decoded (e.g. a fallthrough into a
            # stopped path) get a label that hands over to the dispatcher.
            body = "\n".join(self.out[start:])
            labels = set(re.findall(r"^(L_[0-9A-F]{8}[at]):;", body, re.M))
            missing = sorted(set(re.findall(r"goto (L_[0-9A-F]{8}[at]);", body)) - labels)
            if missing:
                tail = [self.out.pop(), self.out.pop()][::-1]  # "}", ""
                for lab in missing:
                    addr = int(lab[2:10], 16)
                    self.out.append(f"{lab}: rt_jump(c, 0x{addr | (1 if lab[-1] == 't' else 0):08X}u, 0x{addr:08X}u); return;")
                    self.stats["unresolved_fallthrough"] += 1
                self.out.extend(tail)


# --------------------------------------------------------------------------- driver

CHECK_BYTES = 64  # must match the runtime's dispatcher


def residency_check(mod, addr):
    """(length, FNV-1a hash) of the bytes from the function's word-aligned start.

    Overlays loaded at the same address often begin functions with the same
    prologue, so a couple of words can't tell them apart; 64 bytes of code and
    literal pool can. Truncated at the end of the function's segment."""
    start = addr & ~3
    seg = mod.image.find(start)
    length = min(CHECK_BYTES, seg[0] + len(seg[1]) - start)
    h = 0x811C9DC5
    for b in seg[1][start - seg[0]:start - seg[0] + length]:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return (length, h)


def load_modules(layout_path):
    base = os.path.dirname(layout_path)
    layout = json.load(open(layout_path, encoding="utf-8"))
    arm9 = layout["arm9"]

    static_img = Image()
    ranges = []
    for s in arm9["segments"]:
        data = open(os.path.join(base, s["file"]), "rb").read()
        static_img.add(s["addr"], data, s["name"])
        ranges.append((s["addr"], s["addr"] + len(data)))
    modules = [Module("static", -1, static_img, ranges)]

    for ov in layout["overlays9"]:
        img = Image()
        data = open(os.path.join(base, ov["file"]), "rb").read()
        img.add(ov["addr"], data, f"overlay{ov['id']}")
        for seg in static_img.segs:
            img.add(*seg)
        m = Module(f"overlay{ov['id']}", ov["id"], img, [(ov["addr"], ov["addr"] + len(data))])
        m.sinit = ov["sinit"]
        modules.append(m)
    return layout, modules


def overlay_seeds(mod):
    """Static initialisers listed in the overlay's sinit table."""
    seeds = set()
    start, end = mod.sinit
    for a in range(start, end, 4):
        v = mod.image.u32(a)
        if v:
            seeds.add((v & ~1, bool(v & 1)))
    return seeds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only-static", action="store_true")
    ap.add_argument("--seeds", action="append", default=[],
                    help="extra entry points, one hex address per line (bit 0 = Thumb); e.g. dispatch misses")
    args = ap.parse_args()

    layout, modules = load_modules(args.layout)
    if args.only_static:
        modules = modules[:1]
    static = modules[0]

    seeds = set()  # run-time observations: weak entries (see analyse)
    for path in args.seeds:
        for line in open(path, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                v = int(line, 16)
                seeds.add((v & ~1, bool(v & 1)))

    # Static module first, then overlays (whose code calls into the static module).
    analyse(static, {(layout["arm9"]["entry"], False)}, seeds)
    for mod in modules[1:]:
        analyse(mod, overlay_seeds(mod), seeds)
    # Calls from overlays into static functions that the static pass didn't see.
    extra = set()
    for mod in modules[1:]:
        for call in mod.external_calls:
            if static.owns(call[0]) and call not in static.functions:
                extra.add(call)
    if extra:
        analyse(static, extra)

    os.makedirs(args.out, exist_ok=True)
    stats = {"ops": collections.Counter(), "unimpl": collections.Counter(), "instructions": 0,
             "dispatch_calls": 0, "dispatch_jumps": 0, "tail_calls": 0, "exception_returns": 0,
             "unresolved_fallthrough": 0}
    report = {"modules": []}
    files = []
    table = []
    code_ranges = []
    for mod in modules:
        good = [f for f in mod.functions.values() if not f.bad]
        bad = [f for f in mod.functions.values() if f.bad]
        good.sort(key=lambda f: (f.addr, f.thumb))
        em = Emitter(mod, modules, stats)
        em.emit_module(good)

        # Split into files of manageable size.
        chunk, part = [], 0
        header = ['#include "recomp_runtime.h"', '#include "recomp_decls.h"', ""]

        def flush():
            nonlocal chunk, part
            if not chunk:
                return
            path = os.path.join(args.out, f"{mod.prefix()}_{part:03d}.c")
            text = "\n".join(header + chunk) + "\n"
            try:
                if open(path, encoding="utf-8").read() == text:
                    files.append(path)
                    chunk, part = [], part + 1
                    return
            except FileNotFoundError:
                pass
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(text)
            files.append(path)
            chunk, part = [], part + 1

        for line in em.out:
            chunk.append(line)
            if len(chunk) > FILE_SPLIT_LINES and line == "":
                flush()
        flush()

        for f in good:
            check = residency_check(mod, f.addr)
            flags = RECOMP_FLAG_CALLER_FRAME if pops_unpushed_pc(f) else 0
            table.append((f.addr | (1 if f.thumb else 0), mod.overlay_id, check, flags, mod.fn_name(f.addr, f.thumb)))
        code_bytes = sum(e - s for s, e in mod.code_ranges)
        covered = set()
        for f in good:
            for (a, t), ins in f.instrs.items():
                covered.update(range(a, a + ins.size))
        # Merged instruction-byte ranges this module owns (writes there would be self-modifying code).
        start = prev = None
        for b in sorted(x for x in covered if mod.owns(x)):
            if start is None:
                start = prev = b
            elif b == prev + 1:
                prev = b
            else:
                code_ranges.append((mod.overlay_id, start, prev + 1))
                start = prev = b
        if start is not None:
            code_ranges.append((mod.overlay_id, start, prev + 1))
        report["modules"].append({
            "name": mod.name, "functions": len(good), "failed": len(bad),
            "code_bytes": code_bytes,
            "bytes_covered": len([b for b in covered if mod.owns(b)]),
            "failures": [{"addr": f"{f.addr:08X}", "thumb": f.thumb, "reason": f.bad} for f in bad[:50]],
        })

    # Declarations and dispatch table.
    table.sort()
    with open(os.path.join(args.out, "recomp_decls.h"), "w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n#include \"recomp_runtime.h\"\n")
        for *_, name in table:
            f.write(f"void {name}(Cpu* c);\n")
    with open(os.path.join(args.out, "recomp_table.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write('#include "recomp_decls.h"\n\nconst RecompEntry recomp_table[] = {\n')
        for addr, ov, check, flags, name in table:
            f.write(f"    {{0x{addr:08X}u, {ov}, {check[0]}u, 0x{check[1]:08X}u, {flags}u, {name}}},\n")
        f.write(f"}};\nconst unsigned recomp_table_size = {len(table)};\n\n")
        f.write("const RecompCodeRange recomp_code_ranges[] = {\n")
        for ov, a, b in sorted(code_ranges, key=lambda r: (r[1], r[0])):
            f.write(f"    {{{ov}, 0x{a:08X}u, 0x{b:08X}u}},\n")
        f.write(f"}};\nconst unsigned recomp_code_ranges_size = {len(code_ranges)};\n")
    files.append(os.path.join(args.out, "recomp_table.c"))

    # Overlay images for tools that need to place an overlay in memory (difftest).
    base = os.path.dirname(os.path.abspath(args.layout))
    with open(os.path.join(args.out, "overlays.txt"), "w", encoding="utf-8", newline="\n") as f:
        for ov in layout["overlays9"]:
            if any(m.overlay_id == ov["id"] for m in modules):
                f.write(f"{ov['id']} {ov['addr']:08X} {ov['bss_size']} {os.path.join(base, ov['file'])}\n")

    with open(os.path.join(args.out, "sources.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(os.path.basename(p) for p in files) + "\n")

    report["totals"] = {k: v for k, v in stats.items() if not isinstance(v, collections.Counter)}
    report["unimplemented"] = dict(stats["unimpl"].most_common())
    report["op_histogram"] = dict(stats["ops"].most_common())
    with open(os.path.join(args.out, "report.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(report, f, indent=1)

    tot_fn = sum(m["functions"] for m in report["modules"])
    tot_bad = sum(m["failed"] for m in report["modules"])
    cov = sum(m["bytes_covered"] for m in report["modules"])
    code = sum(m["code_bytes"] for m in report["modules"])
    print(f"[recomp] {len(modules)} modules, {tot_fn} functions ({tot_bad} failed), "
          f"{stats['instructions']} instructions, {cov * 100 / max(code, 1):.1f}% of code bytes reached")
    print(f"[recomp] dispatch: {stats['dispatch_calls']} calls, {stats['dispatch_jumps']} jumps, "
          f"{stats['unresolved_fallthrough']} unresolved fallthroughs; unimplemented: {sum(stats['unimpl'].values())}")


if __name__ == "__main__":
    main()
