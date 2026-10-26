#!/usr/bin/env python3
"""Asset pipeline for the native PC build.

Subcommands:
  pack     Validate the .nds image, trim the padding, extract the banner icon
           and title, and emit everything the Windows resource script needs
           (game.bin, game.ico, rom_info.h, game.rc).
  extract  Dump ARM9/ARM7 binaries, overlays (BLZ-decompressed) and the whole
           NitroFS tree. Not embedded in the .exe: this is the starting point
           for a future static recompilation of the game code.
"""
import argparse
import json
import os
import struct
import sys
import zlib


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


class Rom:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 0x200:
            sys.exit(f"error: {path} is too small to be a DS ROM")
        self.title = d[0:12].rstrip(b"\0").decode("ascii", "replace")
        self.gamecode = d[12:16].decode("ascii", "replace")
        self.unitcode = d[0x12]
        (self.arm9_off, self.arm9_entry, self.arm9_ram, self.arm9_size,
         self.arm7_off, self.arm7_entry, self.arm7_ram, self.arm7_size,
         self.fnt_off, self.fnt_size, self.fat_off, self.fat_size,
         self.ov9_off, self.ov9_size, self.ov7_off, self.ov7_size) = struct.unpack_from("<16I", d, 0x20)
        self.banner_off = struct.unpack_from("<I", d, 0x68)[0]
        self.used_size = struct.unpack_from("<I", d, 0x80)[0]
        hcrc = struct.unpack_from("<H", d, 0x15E)[0]
        if crc16(d[:0x15E]) != hcrc:
            sys.exit("error: header CRC mismatch, the ROM is corrupt or not a DS ROM")

    def fat(self):
        n = self.fat_size // 8
        return [struct.unpack_from("<II", self.data, self.fat_off + i * 8) for i in range(n)]

    def trimmed_size(self):
        end = max(self.used_size, self.banner_off + 0xA00)
        for start, stop in self.fat():
            if stop > start:
                end = max(end, stop)
        if self.unitcode & 2:
            twl_size = struct.unpack_from("<I", self.data, 0x210)[0]
            end = max(end, twl_size)
        end = (end + 3) & ~3
        return min(end, len(self.data))

    def banner_title(self, lang=1):
        if not self.banner_off:
            return None
        raw = self.data[self.banner_off + 0x240 + lang * 0x100:self.banner_off + 0x340 + lang * 0x100]
        text = raw.decode("utf-16-le", "replace").split("\0")[0]
        return [line.strip() for line in text.split("\n") if line.strip()]

    def banner_icon_rgba(self):
        """Returns the 32x32 banner icon as rows of RGBA tuples (top to bottom)."""
        b = self.banner_off
        tiles = self.data[b + 0x20:b + 0x220]
        pal = struct.unpack_from("<16H", self.data, b + 0x220)
        colors = []
        for i, c in enumerate(pal):
            r, g, bl = c & 31, (c >> 5) & 31, (c >> 10) & 31
            colors.append(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (bl << 3) | (bl >> 2), 0 if i == 0 else 255))
        px = [[None] * 32 for _ in range(32)]
        for t in range(16):
            tx, ty = t % 4, t // 4
            for y in range(8):
                for x in range(8):
                    byte = tiles[t * 32 + y * 4 + x // 2]
                    idx = (byte >> 4) if x & 1 else (byte & 15)
                    px[ty * 8 + y][tx * 8 + x] = colors[idx]
        return px


def write_ico(path, px, scale=8):
    """Writes a single-image PNG-compressed .ico, upscaled with nearest neighbour."""
    size = 32 * scale
    rows = []
    for y in range(size):
        row = bytearray([0])
        for x in range(size):
            row += bytes(px[y // scale][x // scale])
        rows.append(bytes(row))

    def chunk(tag, payload):
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rows), 9))
    png += chunk(b"IEND", b"")
    dim = 0 if size >= 256 else size
    header = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(png), 22)
    with open(path, "wb") as f:
        f.write(header + png)


def write_if_changed(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    try:
        with open(path, "rb" if mode == "wb" else "r", encoding=None if mode == "wb" else "utf-8") as f:
            if f.read() == data:
                return
    except FileNotFoundError:
        pass
    with open(path, mode, **({} if mode == "wb" else {"encoding": "utf-8", "newline": "\n"})) as f:
        f.write(data)


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def cmd_pack(args):
    rom = Rom(args.rom)
    os.makedirs(args.out, exist_ok=True)
    size = rom.trimmed_size()
    payload = rom.data[:size]
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    title_lines = rom.banner_title() or [rom.title]

    write_if_changed(os.path.join(args.out, "game.bin"), payload)
    write_ico(os.path.join(args.out, "game.ico"), rom.banner_icon_rgba())

    write_if_changed(os.path.join(args.out, "rom_info.h"), "\n".join([
        "// Generated by tools/pack_rom.py - do not edit.",
        "#pragma once",
        f"#define ROM_GAME_CODE {c_string(rom.gamecode)}",
        f"#define ROM_HEADER_TITLE {c_string(rom.title)}",
        f"#define ROM_BANNER_TITLE {c_string(' - '.join(title_lines))}",
        f"#define ROM_EMBEDDED_SIZE {size}u",
        f"#define ROM_ORIGINAL_SIZE {len(rom.data)}u",
        f"#define ROM_EMBEDDED_CRC32 0x{crc:08X}u",
        "",
    ]))

    # windres resolves relative paths against the .rc file's directory.
    write_if_changed(os.path.join(args.out, "game.rc"), "\n".join([
        "// Generated by tools/pack_rom.py - do not edit.",
        "#include <winver.h>",
        "1 ICON \"game.ico\"",
        "ROM_DATA RCDATA \"game.bin\"",
        "",
        "1 VERSIONINFO",
        "FILEVERSION 1,0,0,0",
        "PRODUCTVERSION 1,0,0,0",
        "BEGIN",
        "  BLOCK \"StringFileInfo\"",
        "  BEGIN",
        "    BLOCK \"041604B0\"",
        "    BEGIN",
        f"      VALUE \"FileDescription\", {c_string(args.name)}",
        f"      VALUE \"ProductName\", {c_string(args.name)}",
        "      VALUE \"FileVersion\", \"1.0.0\"",
        "      VALUE \"ProductVersion\", \"1.0.0\"",
        "      VALUE \"Comments\", \"Built with the melonDS core (GPLv3)\"",
        "    END",
        "  END",
        "  BLOCK \"VarFileInfo\"",
        "  BEGIN",
        "    VALUE \"Translation\", 0x0416, 1200",
        "  END",
        "END",
        "",
    ]))

    print(f"[pack_rom] {rom.gamecode} '{rom.title}' ({' / '.join(title_lines)})")
    print(f"[pack_rom] embedded {size} of {len(rom.data)} bytes, crc32 {crc:08X}")


def blz_decompress(buf):
    """Nintendo backwards-LZ (used by ARM9 main binary and overlays)."""
    if len(buf) < 8:
        return buf
    hdr_len = buf[-5]
    comp_len = struct.unpack_from("<I", buf, len(buf) - 8)[0] & 0xFFFFFF
    extra = struct.unpack_from("<I", buf, len(buf) - 4)[0]
    if hdr_len < 8 or comp_len > len(buf):
        return buf
    out = bytearray(buf) + bytearray(extra)
    src = len(buf) - hdr_len
    dst = len(out)
    stop = len(buf) - comp_len
    while src > stop:
        src -= 1
        flags = buf[src]
        for _ in range(8):
            if src <= stop:
                break
            if flags & 0x80:
                src -= 2
                pair = buf[src] | (buf[src + 1] << 8)
                disp = (pair & 0xFFF) + 3
                count = (pair >> 12) + 3
                for _ in range(count):
                    dst -= 1
                    out[dst] = out[dst + disp]
            else:
                src -= 1
                dst -= 1
                out[dst] = buf[src]
            flags <<= 1
    return bytes(out)


NITROCODE = struct.pack("<II", 0xDEC00621, 0x2106C0DE)


def split_arm9(image, ram_addr):
    """Decompresses the ARM9 static module and splits off its autoload sections.

    Returns (main_image, segments, params) where segments are the autoload
    blocks (ITCM, DTCM, ...) that crt0 copies to their own addresses at boot.
    """
    pos = image.find(NITROCODE)
    if pos < 0x1C:
        sys.exit("error: ARM9 module params (nitrocode) not found")
    pofs = pos - 0x1C
    (al_list_start, al_list_end, al_start, bss_start, bss_end,
     comp_end, sdk_version) = struct.unpack_from("<7I", image, pofs)

    if comp_end:
        image = blz_decompress(image[:comp_end - ram_addr]) + image[comp_end - ram_addr:]
        image = bytearray(image)
        struct.pack_into("<I", image, pofs + 0x14, 0)  # mark as decompressed, like crt0 does
        image = bytes(image)

    segments = []
    src = al_start - ram_addr
    # SDK 5 (TWL-aware) entries are {dest, size, sinit, bss}; older SDKs drop sinit.
    stride = 16 if (sdk_version >> 24) >= 5 else 12
    for entry in range((al_list_end - al_list_start) // stride):
        fields = struct.unpack_from("<4I" if stride == 16 else "<3I", image, al_list_start - ram_addr + entry * stride)
        dest, size, bss = fields[0], fields[1], fields[-1]
        segments.append({"addr": dest, "size": size, "bss_size": bss, "data": image[src:src + size]})
        src += size

    params = {"sdk_version": f"0x{sdk_version:08X}", "bss_start": bss_start, "bss_end": bss_end,
              "was_compressed": bool(comp_end), "autoload_list": [al_list_start, al_list_end]}
    return image[:al_start - ram_addr], segments, params


def cmd_extract(args):
    rom = Rom(args.rom)
    d = rom.data
    out = args.out
    os.makedirs(os.path.join(out, "overlay"), exist_ok=True)
    fat = rom.fat()

    def save(rel, blob):
        with open(os.path.join(out, rel), "wb") as f:
            f.write(blob)

    arm9_raw = d[rom.arm9_off:rom.arm9_off + rom.arm9_size]
    arm9_main, autoloads, arm9_params = split_arm9(arm9_raw, rom.arm9_ram)
    save("arm9.bin", arm9_main)
    arm9_segments = [{"name": "arm9_static", "file": "arm9.bin", "addr": rom.arm9_ram, "size": len(arm9_main),
                      "bss_start": arm9_params["bss_start"], "bss_end": arm9_params["bss_end"]}]
    for i, seg in enumerate(autoloads):
        name = f"arm9_autoload{i}"
        save(name + ".bin", seg["data"])
        arm9_segments.append({"name": name, "file": name + ".bin", "addr": seg["addr"], "size": seg["size"],
                              "bss_start": seg["addr"] + seg["size"],
                              "bss_end": seg["addr"] + seg["size"] + seg["bss_size"]})

    save("arm7.bin", d[rom.arm7_off:rom.arm7_off + rom.arm7_size])

    overlays = []
    for i in range(rom.ov9_size // 32):
        ov_id, ram, ram_size, bss, sinit_start, sinit_end, file_id, flags = \
            struct.unpack_from("<8I", d, rom.ov9_off + i * 32)
        start, stop = fat[file_id]
        blob = d[start:stop]
        compressed = bool(flags & 0x01000000)
        if compressed:
            blob = blz_decompress(blob)
        rel = f"overlay/overlay9_{ov_id:04d}.bin"
        save(rel, blob)
        overlays.append({"id": ov_id, "file": rel, "file_id": file_id, "addr": ram, "size": len(blob),
                         "bss_size": bss, "sinit": [sinit_start, sinit_end], "was_compressed": compressed})

    layout = {
        "game_code": rom.gamecode,
        "arm9": {"entry": rom.arm9_entry, "params": arm9_params, "segments": arm9_segments},
        "arm7": {"entry": rom.arm7_entry, "segments": [
            {"name": "arm7_static", "file": "arm7.bin", "addr": rom.arm7_ram, "size": rom.arm7_size}]},
        "overlays9": overlays,
    }
    with open(os.path.join(out, "layout.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(layout, f, indent=1)

    # NitroFS: walk the FNT directory table.
    count = 0

    def walk(dir_id, path):
        nonlocal count
        base = rom.fnt_off + (dir_id & 0xFFF) * 8
        sub_off, first_file, _ = struct.unpack_from("<IHH", d, base)
        p = rom.fnt_off + sub_off
        file_id = first_file
        while True:
            t = d[p]
            p += 1
            if t == 0:
                break
            name = d[p:p + (t & 0x7F)].decode("latin-1")
            p += t & 0x7F
            if t & 0x80:
                sub = struct.unpack_from("<H", d, p)[0]
                p += 2
                walk(sub, os.path.join(path, name))
            else:
                start, stop = fat[file_id]
                dest = os.path.join(out, "data", path, name)
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                with open(dest, "wb") as f:
                    f.write(d[start:stop])
                file_id += 1
                count += 1

    walk(0xF000, "")
    print(f"[extract] arm9 {len(arm9_main)} B + {len(autoloads)} autoload, arm7 {rom.arm7_size} B, "
          f"{rom.ov9_size // 32} overlays, {count} NitroFS files -> {out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--rom", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--name", required=True)
    p.set_defaults(func=cmd_pack)
    e = sub.add_parser("extract")
    e.add_argument("--rom", required=True)
    e.add_argument("--out", required=True)
    e.set_defaults(func=cmd_extract)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
