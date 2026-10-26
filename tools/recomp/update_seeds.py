#!/usr/bin/env python3
"""Adds run-time dispatch misses to the recompiler's seed list.

  recomp_difftest <rom> --overlays build/recomp/overlays.txt --unknown build/misses.txt
  python -I tools/recomp/update_seeds.py --layout build/extracted/layout.json \
      --misses build/misses.txt --seeds tools/recomp/seeds_IRBO.txt --table build/recomp/recomp_table.c

A miss becomes a seed only if it lies in recompiled code, decodes as plausible
code, and isn't a return address (the instruction right after a BL/BLX):
returns that go through the dispatcher are recompiler bugs, not new functions.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recomp  # noqa: E402

HEADER = ("# Entry points observed at run time (recomp_difftest --unknown) that static analysis\n"
          "# can't find: function pointers computed by code. Bit 0 set = Thumb.\n"
          "# Maintained by tools/recomp/update_seeds.py.\n")


def follows_call(mod, addr, thumb):
    for size in ((4, 2) if thumb else (4,)):
        prev = mod.image.decode(addr - size, thumb)
        if prev is not None and prev.size == size and prev.op in ("b", "bx") and prev.link:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layout", required=True)
    ap.add_argument("--misses", required=True)
    ap.add_argument("--seeds", required=True)
    ap.add_argument("--table", help="recomp_table.c: misses already recompiled (in a non-resident overlay) are skipped")
    args = ap.parse_args()

    known = set()
    if args.table:
        known = {int(m, 16) for m in re.findall(r"\{0x([0-9A-F]{8})u,", open(args.table, encoding="utf-8").read())}

    _, modules = recomp.load_modules(args.layout)
    seeds = set()
    if os.path.exists(args.seeds):
        for line in open(args.seeds, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                seeds.add(int(line, 16))

    added = rejected = 0
    for line in open(args.misses, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        v = int(line, 16)
        if v in seeds or v in known:
            continue
        addr, thumb = v & ~1, bool(v & 1)
        mod = next((m for m in modules if m.owns(addr)), None)
        if mod is None or follows_call(mod, addr, thumb) or not recomp.plausible_code(mod, addr, thumb):
            rejected += 1
            continue
        seeds.add(v)
        added += 1

    with open(args.seeds, "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER)
        for v in sorted(seeds):
            f.write(f"{v:08X}\n")
    print(f"[seeds] {added} added, {rejected} misses rejected, {len(seeds)} total")


if __name__ == "__main__":
    main()
