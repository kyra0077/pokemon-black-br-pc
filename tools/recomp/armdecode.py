"""ARMv5TE decoder (ARM9 / ARM946E-S) for the static recompiler.

ARM and Thumb encodings decode into the same Instr representation so the C
emitter only implements each operation once. Thumb instructions are expressed
as their ARM equivalents (with set_flags matching Thumb behaviour).
"""
import struct

COND_NAMES = ["eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "", "nv"]
DP_OPS = ["and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc", "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"]
SHIFTS = ["lsl", "lsr", "asr", "ror"]
AL = 14
PC, LR, SP = 15, 14, 13


class Instr:
    __slots__ = ("addr", "size", "thumb", "cond", "op", "rd", "rn", "rm", "rs", "ra",
                 "imm", "op2_imm", "shift", "shift_imm", "shift_reg", "set_flags",
                 "pre", "up", "writeback", "load", "width", "regs", "user",
                 "target", "link", "exchange", "extra", "raw")

    def __init__(self, addr, size, thumb, raw, op, cond=AL, **kw):
        self.addr, self.size, self.thumb, self.raw, self.op, self.cond = addr, size, thumb, raw, op, cond
        self.rd = self.rn = self.rm = self.rs = self.ra = None
        self.imm = 0
        self.op2_imm = False
        self.shift = 0
        self.shift_imm = 0
        self.shift_reg = False
        self.set_flags = False
        self.pre = self.up = self.writeback = self.load = False
        self.width = "w"
        self.regs = 0
        self.user = False
        self.target = None
        self.link = False
        self.exchange = False
        self.extra = None
        for k, v in kw.items():
            setattr(self, k, v)

    # PC value seen by this instruction when it reads r15.
    @property
    def pc_read(self):
        return self.addr + (4 if self.thumb else 8)

    def reg_list(self):
        return [r for r in range(16) if self.regs >> r & 1]

    def __repr__(self):
        return f"{self.addr:08X}: {disasm(self)}"


def ror(v, s):
    s &= 31
    return ((v >> s) | (v << (32 - s))) & 0xFFFFFFFF if s else v


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


# --------------------------------------------------------------------------- ARM

def _dp(addr, i, cond):
    op = DP_OPS[(i >> 21) & 15]
    ins = Instr(addr, 4, False, i, op, cond, rd=(i >> 12) & 15, rn=(i >> 16) & 15, set_flags=bool(i >> 20 & 1))
    if i >> 25 & 1:
        rot = ((i >> 8) & 15) * 2
        ins.op2_imm = True
        ins.imm = ror(i & 0xFF, rot)
        ins.shift_imm = rot  # needed for the shifter carry-out of rotated immediates
    else:
        ins.rm = i & 15
        ins.shift = (i >> 5) & 3
        if i >> 4 & 1:
            ins.shift_reg = True
            ins.rs = (i >> 8) & 15
        else:
            ins.shift_imm = (i >> 7) & 31
    if op in ("tst", "teq", "cmp", "cmn"):
        ins.rd = None
        ins.set_flags = True
    if op in ("mov", "mvn"):
        ins.rn = None
    return ins


def decode_arm(addr, i):
    cond = i >> 28
    if cond == 0xF:
        if (i & 0x0E000000) == 0x0A000000:  # BLX imm
            off = sext(i & 0xFFFFFF, 24) * 4 + ((i >> 24) & 1) * 2
            return Instr(addr, 4, False, i, "b", AL, target=(addr + 8 + off) & 0xFFFFFFFF, link=True, exchange=True)
        if (i & 0x0D70F000) == 0x0550F000:
            return Instr(addr, 4, False, i, "nop", AL, extra="pld")
        return Instr(addr, 4, False, i, "undef", AL)

    bits = (i >> 25) & 7
    if bits == 0:
        if (i & 0x0FFFFFD0) == 0x012FFF10:
            return Instr(addr, 4, False, i, "bx", cond, rm=i & 15, link=bool(i & 0x20), exchange=True)
        if (i & 0x0FFF0FF0) == 0x016F0F10:
            return Instr(addr, 4, False, i, "clz", cond, rd=(i >> 12) & 15, rm=i & 15)
        if (i & 0x0F900FF0) == 0x01000050:
            return Instr(addr, 4, False, i, ["qadd", "qsub", "qdadd", "qdsub"][(i >> 21) & 3], cond,
                         rd=(i >> 12) & 15, rn=(i >> 16) & 15, rm=i & 15)
        if (i & 0x0F900090) == 0x01000080:
            kind = (i >> 21) & 3
            x, y = (i >> 5) & 1, (i >> 6) & 1
            names = {0: "smla", 1: "smlaw" if not x else "smulw", 2: "smlal", 3: "smul"}
            op = names[kind]
            ins = Instr(addr, 4, False, i, op, cond, rd=(i >> 16) & 15, ra=(i >> 12) & 15, rs=(i >> 8) & 15, rm=i & 15,
                        extra=(x, y))
            if op == "smlal":
                ins.ra, ins.rd = (i >> 12) & 15, (i >> 16) & 15  # RdLo, RdHi
            return ins
        if (i & 0x0FBF0FFF) == 0x010F0000:
            return Instr(addr, 4, False, i, "mrs", cond, rd=(i >> 12) & 15, extra=bool(i >> 22 & 1))
        if (i & 0x0FB0FFF0) == 0x0120F000:
            return Instr(addr, 4, False, i, "msr", cond, rm=i & 15, imm=(i >> 16) & 15, extra=bool(i >> 22 & 1))
        if (i & 0x0FC000F0) == 0x00000090:
            acc = bool(i >> 21 & 1)
            return Instr(addr, 4, False, i, "mla" if acc else "mul", cond, rd=(i >> 16) & 15, ra=(i >> 12) & 15,
                         rs=(i >> 8) & 15, rm=i & 15, set_flags=bool(i >> 20 & 1))
        if (i & 0x0F8000F0) == 0x00800090:
            op = ["umull", "umlal", "smull", "smlal_l"][(i >> 21) & 3]
            return Instr(addr, 4, False, i, op, cond, rd=(i >> 16) & 15, ra=(i >> 12) & 15,
                         rs=(i >> 8) & 15, rm=i & 15, set_flags=bool(i >> 20 & 1))
        if (i & 0x0FB00FF0) == 0x01000090:
            return Instr(addr, 4, False, i, "swp", cond, rd=(i >> 12) & 15, rn=(i >> 16) & 15, rm=i & 15,
                         width="b" if i >> 22 & 1 else "w")
        if (i & 0x0E000090) == 0x00000090 and (i >> 5) & 3:
            sh = (i >> 5) & 3
            load = bool(i >> 20 & 1)
            if load:
                width = {1: "h", 2: "sb", 3: "sh"}[sh]
            else:
                width = {1: "h", 2: "ldrd", 3: "strd"}[sh]
            op = "ldr" if load or width == "ldrd" else "str"
            if width == "ldrd":
                op, width = "ldr", "d"
            elif width == "strd":
                op, width = "str", "d"
            ins = Instr(addr, 4, False, i, op, cond, rd=(i >> 12) & 15, rn=(i >> 16) & 15, width=width,
                        pre=bool(i >> 24 & 1), up=bool(i >> 23 & 1), load=(op == "ldr"))
            ins.writeback = (not ins.pre) or bool(i >> 21 & 1)
            if i >> 22 & 1:
                ins.op2_imm = True
                ins.imm = ((i >> 4) & 0xF0) | (i & 15)
            else:
                ins.rm = i & 15
            return ins
        if (i & 0x0F900010) == 0x01000000 or (i & 0x0F900090) == 0x01000010:
            return Instr(addr, 4, False, i, "undef", cond)
        return _dp(addr, i, cond)

    if bits == 1:
        if (i & 0x0FB0F000) == 0x0320F000:
            return Instr(addr, 4, False, i, "msr", cond, op2_imm=True, imm=ror(i & 0xFF, ((i >> 8) & 15) * 2),
                         rs=(i >> 16) & 15, extra=bool(i >> 22 & 1))
        if (i & 0x0FB00000) == 0x03000000:
            return Instr(addr, 4, False, i, "undef", cond)
        return _dp(addr, i, cond)

    if bits in (2, 3):
        if bits == 3 and i >> 4 & 1:
            return Instr(addr, 4, False, i, "undef", cond)
        load = bool(i >> 20 & 1)
        ins = Instr(addr, 4, False, i, "ldr" if load else "str", cond, rd=(i >> 12) & 15, rn=(i >> 16) & 15,
                    width="b" if i >> 22 & 1 else "w", pre=bool(i >> 24 & 1), up=bool(i >> 23 & 1), load=load)
        ins.writeback = (not ins.pre) or bool(i >> 21 & 1)
        ins.user = (not ins.pre) and bool(i >> 21 & 1)  # ldrt/strt
        if bits == 2:
            ins.op2_imm = True
            ins.imm = i & 0xFFF
        else:
            ins.rm = i & 15
            ins.shift = (i >> 5) & 3
            ins.shift_imm = (i >> 7) & 31
        return ins

    if bits == 4:
        load = bool(i >> 20 & 1)
        return Instr(addr, 4, False, i, "ldm" if load else "stm", cond, rn=(i >> 16) & 15, regs=i & 0xFFFF,
                     pre=bool(i >> 24 & 1), up=bool(i >> 23 & 1), user=bool(i >> 22 & 1),
                     writeback=bool(i >> 21 & 1), load=load)

    if bits == 5:
        off = sext(i & 0xFFFFFF, 24) * 4
        return Instr(addr, 4, False, i, "b", cond, target=(addr + 8 + off) & 0xFFFFFFFF, link=bool(i >> 24 & 1))

    if bits == 6:
        return Instr(addr, 4, False, i, "undef", cond, extra="ldc/stc")

    # bits == 7
    if i >> 24 & 1:
        return Instr(addr, 4, False, i, "swi", cond, imm=i & 0xFFFFFF)
    if i >> 4 & 1:
        return Instr(addr, 4, False, i, "mrc" if i >> 20 & 1 else "mcr", cond, rd=(i >> 12) & 15,
                     extra=((i >> 8) & 15, (i >> 21) & 7, (i >> 16) & 15, i & 15, (i >> 5) & 7))
    return Instr(addr, 4, False, i, "undef", cond, extra="cdp")


# --------------------------------------------------------------------------- Thumb

def decode_thumb(addr, h, h2=None):
    """h is the halfword at addr; h2 the following one (needed for BL/BLX pairs)."""
    T = lambda op, **kw: Instr(addr, 2, True, h, op, AL, **kw)
    top = h >> 13

    if top == 0:
        if (h >> 11) & 3 != 3:  # LSL/LSR/ASR imm
            return T("mov", rd=h & 7, rm=(h >> 3) & 7, shift=(h >> 11) & 3, shift_imm=(h >> 6) & 31, set_flags=True)
        rn, rd = (h >> 3) & 7, h & 7
        sub = bool(h >> 9 & 1)
        if h >> 10 & 1:
            return T("sub" if sub else "add", rd=rd, rn=rn, op2_imm=True, imm=(h >> 6) & 7, set_flags=True)
        return T("sub" if sub else "add", rd=rd, rn=rn, rm=(h >> 6) & 7, set_flags=True)

    if top == 1:  # mov/cmp/add/sub imm8
        op = ["mov", "cmp", "add", "sub"][(h >> 11) & 3]
        rd = (h >> 8) & 7
        ins = T(op, rd=rd, rn=rd, op2_imm=True, imm=h & 0xFF, set_flags=True)
        if op == "mov":
            ins.rn = None
        if op == "cmp":
            ins.rd = None
        return ins

    if (h >> 10) == 0b010000:  # ALU
        op = (h >> 6) & 15
        rd, rm = h & 7, (h >> 3) & 7
        if op in (2, 3, 4, 7):  # lsl, lsr, asr, ror by register
            return T("mov", rd=rd, rm=rd, shift={2: 0, 3: 1, 4: 2, 7: 3}[op], shift_reg=True, rs=rm, set_flags=True)
        if op == 9:  # neg
            return T("rsb", rd=rd, rn=rm, op2_imm=True, imm=0, set_flags=True)
        if op == 13:
            return T("mul", rd=rd, rm=rd, rs=rm, set_flags=True)
        name = {0: "and", 1: "eor", 5: "adc", 6: "sbc", 8: "tst", 10: "cmp", 11: "cmn", 12: "orr", 14: "bic", 15: "mvn"}[op]
        ins = T(name, rd=rd, rn=rd, rm=rm, set_flags=True)
        if name in ("tst", "cmp", "cmn"):
            ins.rd = None
        if name == "mvn":
            ins.rn = None
        return ins

    if (h >> 10) == 0b010001:  # hi register ops / bx
        op = (h >> 8) & 3
        rd = (h & 7) | ((h >> 4) & 8)
        rm = (h >> 3) & 15
        if op == 0:
            return T("add", rd=rd, rn=rd, rm=rm)
        if op == 1:
            return T("cmp", rn=rd, rm=rm, set_flags=True)
        if op == 2:
            return T("mov", rd=rd, rm=rm)
        return T("bx", rm=rm, link=bool(h >> 7 & 1), exchange=True)

    if (h >> 11) == 0b01001:  # ldr rd, [pc, #imm]
        return T("ldr", rd=(h >> 8) & 7, rn=PC, op2_imm=True, imm=(h & 0xFF) * 4, pre=True, up=True, load=True)

    if (h >> 12) == 0b0101:  # load/store register offset
        op = (h >> 9) & 7
        width, load = [("w", False), ("h", False), ("b", False), ("sb", True),
                       ("w", True), ("h", True), ("b", True), ("sh", True)][op]
        return T("ldr" if load else "str", rd=h & 7, rn=(h >> 3) & 7, rm=(h >> 6) & 7, pre=True, up=True,
                 load=load, width=width)

    if (h >> 13) == 0b011:  # ldr/str imm (word/byte)
        byte = bool(h >> 12 & 1)
        load = bool(h >> 11 & 1)
        imm = (h >> 6) & 31
        return T("ldr" if load else "str", rd=h & 7, rn=(h >> 3) & 7, op2_imm=True, imm=imm if byte else imm * 4,
                 pre=True, up=True, load=load, width="b" if byte else "w")

    if (h >> 12) == 0b1000:  # ldrh/strh imm
        load = bool(h >> 11 & 1)
        return T("ldr" if load else "str", rd=h & 7, rn=(h >> 3) & 7, op2_imm=True, imm=((h >> 6) & 31) * 2,
                 pre=True, up=True, load=load, width="h")

    if (h >> 12) == 0b1001:  # sp-relative
        load = bool(h >> 11 & 1)
        return T("ldr" if load else "str", rd=(h >> 8) & 7, rn=SP, op2_imm=True, imm=(h & 0xFF) * 4,
                 pre=True, up=True, load=load)

    if (h >> 12) == 0b1010:  # add rd, pc/sp, #imm
        src = SP if h >> 11 & 1 else PC
        return T("add", rd=(h >> 8) & 7, rn=src, op2_imm=True, imm=(h & 0xFF) * 4)

    if (h >> 8) == 0b10110000:  # add/sub sp, #imm
        return T("sub" if h >> 7 & 1 else "add", rd=SP, rn=SP, op2_imm=True, imm=(h & 0x7F) * 4)

    if (h >> 12) == 0b1011 and ((h >> 9) & 3) == 2:  # push/pop
        load = bool(h >> 11 & 1)
        regs = h & 0xFF
        if h >> 8 & 1:
            regs |= (1 << PC) if load else (1 << LR)
        if load:
            return T("ldm", rn=SP, regs=regs, pre=False, up=True, writeback=True, load=True)
        return T("stm", rn=SP, regs=regs, pre=True, up=False, writeback=True)

    if (h >> 8) == 0b10111110:
        return T("bkpt", imm=h & 0xFF)

    if (h >> 12) == 0b1100:  # ldmia/stmia
        load = bool(h >> 11 & 1)
        rn = (h >> 8) & 7
        regs = h & 0xFF
        # ldmia with rn in the list does not write back
        wb = not (load and (regs >> rn & 1))
        return T("ldm" if load else "stm", rn=rn, regs=regs, pre=False, up=True, writeback=wb, load=load)

    if (h >> 12) == 0b1101:
        cond = (h >> 8) & 15
        if cond == 15:
            return T("swi", imm=h & 0xFF)
        if cond == 14:
            return T("undef")
        ins = T("b", target=(addr + 4 + sext(h & 0xFF, 8) * 2) & 0xFFFFFFFF)
        ins.cond = cond
        return ins

    if (h >> 11) == 0b11100:
        return T("b", target=(addr + 4 + sext(h & 0x7FF, 11) * 2) & 0xFFFFFFFF)

    if (h >> 11) == 0b11110:  # BL/BLX prefix
        if h2 is not None and (h2 >> 11) in (0b11111, 0b11101):
            off = (sext(h & 0x7FF, 11) << 12) | ((h2 & 0x7FF) << 1)
            target = (addr + 4 + off) & 0xFFFFFFFF
            blx = (h2 >> 11) == 0b11101
            if blx:
                target &= ~3
            return Instr(addr, 4, True, h | (h2 << 16), "b", AL, target=target, link=True, exchange=blx)
        return T("undef", extra="lone bl prefix")

    return T("undef")


# --------------------------------------------------------------------------- disassembly text

def _op2(ins):
    if ins.op2_imm:
        return f"#0x{ins.imm:X}"
    s = f"r{ins.rm}"
    if ins.shift_reg:
        return f"{s}, {SHIFTS[ins.shift]} r{ins.rs}"
    if ins.shift_imm or ins.shift:
        amt = ins.shift_imm if ins.shift_imm or ins.shift == 0 else 32
        if ins.shift == 3 and ins.shift_imm == 0:
            return f"{s}, rrx"
        return f"{s}, {SHIFTS[ins.shift]} #{amt}"
    return s


def _rl(regs):
    return "{" + ", ".join(f"r{r}" for r in range(16) if regs >> r & 1) + "}"


def disasm(ins):
    c = COND_NAMES[ins.cond]
    op = ins.op
    if op in DP_OPS:
        s = "s" if ins.set_flags and op not in ("tst", "teq", "cmp", "cmn") else ""
        parts = [f"r{r}" for r in (ins.rd, ins.rn) if r is not None]
        return f"{op}{c}{s} " + ", ".join(parts + [_op2(ins)])
    if op in ("ldr", "str"):
        w = {"w": "", "b": "b", "h": "h", "sb": "sb", "sh": "sh", "d": "d"}[ins.width]
        off = f"#{'' if ins.up else '-'}0x{ins.imm:X}" if ins.op2_imm else ("" if ins.up else "-") + _op2(ins)
        if ins.pre:
            return f"{op}{c}{w} r{ins.rd}, [r{ins.rn}, {off}]{'!' if ins.writeback else ''}"
        return f"{op}{c}{w} r{ins.rd}, [r{ins.rn}], {off}"
    if op in ("ldm", "stm"):
        mode = ("i" if ins.up else "d") + ("b" if ins.pre else "a")
        return f"{op}{c}{mode} r{ins.rn}{'!' if ins.writeback else ''}, {_rl(ins.regs)}{'^' if ins.user else ''}"
    if op == "b":
        name = "blx" if ins.link and ins.exchange else ("bl" if ins.link else "b")
        return f"{name}{c} 0x{ins.target:08X}"
    if op == "bx":
        return f"{'blx' if ins.link else 'bx'}{c} r{ins.rm}"
    if op == "swi":
        return f"swi{c} 0x{ins.imm:X}"
    if op in ("mcr", "mrc"):
        cp, op1, crn, crm, op2 = ins.extra
        return f"{op}{c} p{cp}, {op1}, r{ins.rd}, c{crn}, c{crm}, {op2}"
    regs = ", ".join(f"r{r}" for r in (ins.rd, ins.rn, ins.rm, ins.rs, ins.ra) if r is not None)
    return f"{op}{c} {regs}".rstrip()


class Image:
    """Flat view of the loaded segments of one address space (static + one overlay)."""

    def __init__(self):
        self.segs = []  # (start, bytes, name)

    def add(self, start, data, name):
        self.segs.append((start, data, name))

    def find(self, addr, size=1):
        for start, data, name in self.segs:
            if start <= addr and addr + size <= start + len(data):
                return start, data, name
        return None

    def contains(self, addr):
        return self.find(addr) is not None

    def u16(self, addr):
        seg = self.find(addr, 2)
        return struct.unpack_from("<H", seg[1], addr - seg[0])[0] if seg else None

    def u32(self, addr):
        seg = self.find(addr, 4)
        return struct.unpack_from("<I", seg[1], addr - seg[0])[0] if seg else None

    def decode(self, addr, thumb):
        if thumb:
            h = self.u16(addr)
            if h is None:
                return None
            return decode_thumb(addr, h, self.u16(addr + 2))
        w = self.u32(addr)
        return None if w is None else decode_arm(addr, w)
