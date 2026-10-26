"""Writes a copy of melonDS's ARM.cpp with the two hooks the hybrid runtime needs.

The submodule itself is never modified: CMake compiles this copy in place of
the original when ENABLE_HYBRID is on. The hooks are inert (null pointers)
unless src/recomp/hybrid.cpp installs them.

  1. ARMv5::JumpTo reports every branch target, so the runtime can notice a
     jump to the entry of a recompiled function (BL, BX, B, POP {PC}, ...).
  2. ARMv5::Execute (interpreter path) gives the runtime a chance to run the
     recompiled function at the next instruction boundary, when the jump that
     reached it has fully completed (writeback, LR, pipeline refill).

usage: patch_core_hybrid.py <melonDS/src/ARM.cpp> <out/ARM.cpp>
"""
import re
import sys


def sub_once(pattern, repl, text, what):
    new, n = re.subn(pattern, repl, text, count=1)
    if n != 1:
        sys.exit(f"patch_core_hybrid: anchor not found ({what}); melonDS changed?")
    return new


def main():
    src, out = sys.argv[1], sys.argv[2]
    with open(src, newline="") as f:
        text = f.read()
    nl = "\r\n" if "\r\n" in text else "\n"

    decls = nl.join([
        "// ---- hybrid recompilation hooks (added by tools/recomp/patch_core_hybrid.py) ----",
        "void (*HybridJumpHook)(u32 addr) = nullptr;",
        "bool (*HybridRunHook)(ARMv5& cpu) = nullptr;",
        "bool HybridPending = false;",
        "",
        "",
    ])
    text = sub_once(r"void ARMv5::JumpTo\(u32 addr, bool restorecpsr\)\r?\n\{\r?\n",
                    lambda m: decls + m.group(0) +
                    "    if (HybridJumpHook && !restorecpsr) HybridJumpHook(addr);" + nl + nl,
                    text, "ARMv5::JumpTo")

    # Only ARMv5::Execute: ARMv4 (the ARM7) has an identical THUMB line further down.
    start = text.index("void ARMv5::Execute()")
    end = text.index("void ARMv4::Execute()")
    body = text[start:end]
    hook = nl.join([
        "if (HybridPending && HybridRunHook(*this))",
        "            {",
        "                NDS.ARM9Timestamp += Cycles;",
        "                Cycles = 0;",
        "                if (IRQ) TriggerIRQ();",
        "                continue;",
        "            }",
        "",
        "            ",
    ])
    body = sub_once(r"if \(CPSR & 0x20\) // THUMB", lambda m: hook + m.group(0), body, "ARMv5::Execute")
    text = text[:start] + body + text[end:]

    with open(out, "w", newline="") as f:
        f.write(text)


if __name__ == "__main__":
    main()
