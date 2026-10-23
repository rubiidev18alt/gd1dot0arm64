#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Dump structural facts about the original Geometry Dash 1.02 APK.

Used by CI (which is the only place the original APK exists) to report what the
2013 build actually ships: assets, sprite-sheet frame names, the tiny
objectDefinitions.plist, and a survey of libgame.so strings/symbols. The output
is plain text so it can be committed back to the repository and read outside CI.
"""

from __future__ import annotations

import argparse
import plistlib
import re
import struct
import subprocess
import zipfile
import zlib
from pathlib import Path

STRING_PATTERNS = [
    ("portal_0", r"portal_0"),
    ("portal_1", r"portal_1"),
    ("mirror", r"[Mm]irror"),
    ("LevelData", r"LevelData"),
    ("time_machine", r"Time Machine"),
    ("stereo_madness", r"Stereo Madness"),
    ("jumper", r"Jumper"),
    ("level_string_header", r"kS1,"),
    ("compressed_level_gzip", r"H4sI"),
    ("achievement", r"[Aa]chievement"),
    ("max_texture_size", r"GL_MAX_TEXTURE_SIZE|maxTextureSize"),
    ("bitmap_text", r"createTextBitmap|Cocos2dxBitmap"),
]

LEVEL_NAMES = (
    r"stereo|polargeist|dry out|base after|let go|jumper|time machine|cycles|"
    r"xstep|clutterfunk|fingerdash|electroman|hexagon|deadlocked"
)


def _strings(data: bytes, min_len: int = 4) -> list[str]:
    return [m.group().decode("latin-1") for m in re.finditer(rb"[\x20-\x7e]{%d,}" % min_len, data)]


def _elf_symbols(blob: bytes) -> list[str]:
    """Best-effort .dynsym name extraction for the 32-bit guest ELF."""
    if len(blob) < 52 or blob[:4] != b"\x7fELF":
        return []
    is64 = blob[4] == 2
    if is64:
        return []
    e_shoff, = struct.unpack_from("<I", blob, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", blob, 0x2E)
    if not e_shoff or not e_shnum:
        return []
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + 40 > len(blob):
            break
        sections.append(struct.unpack_from("<IIIIIIIIII", blob, off))
    names: list[str] = []
    for sh in sections:
        sh_type, sh_offset, sh_size, sh_link, sh_entsize = sh[1], sh[4], sh[5], sh[6], sh[9]
        if sh_type != 11 or sh_entsize != 16:  # SHT_DYNSYM
            continue
        strtab = sections[sh_link]
        str_off, str_size = strtab[4], strtab[5]
        strtab_bytes = blob[str_off:str_off + str_size]
        for off in range(sh_offset, min(sh_offset + sh_size, len(blob)), 16):
            st_name, = struct.unpack_from("<I", blob, off)
            if not st_name or st_name >= len(strtab_bytes):
                continue
            end = strtab_bytes.find(b"\x00", st_name)
            if end < 0:
                continue
            name = strtab_bytes[st_name:end].decode("latin-1", "replace")
            if name:
                names.append(name)
    return names


def dump(apk: Path, out: Path) -> None:
    lines: list[str] = []
    add = lines.append

    with zipfile.ZipFile(apk) as zf:
        infos = zf.infolist()
        add(f"# {apk.name} facts")
        add(f"apk_size {apk.stat().st_size}")
        add("")
        add("## entries")
        for info in sorted(infos, key=lambda i: i.filename):
            add(f"{info.file_size} {info.filename}")

        assets = [i.filename for i in infos if i.filename.startswith("assets/")]
        add("")
        add(f"## counts assets={len(assets)} total_entries={len(infos)}")
        add(f"level_data_plist {'yes' if 'assets/LevelData.plist' in assets else 'no'}")

        def read(name: str) -> bytes | None:
            try:
                return zf.read(name)
            except KeyError:
                return None

        objdef = read("assets/objectDefinitions.plist")
        add("")
        add("## objectDefinitions.plist")
        if objdef is None:
            add("missing")
        else:
            add(f"raw_size {len(objdef)}")
            try:
                d = plistlib.loads(objdef)
                add(repr(d))
            except Exception as exc:  # noqa: BLE001
                add(f"unparsable: {exc!r}")
                add(objdef[:400].decode("latin-1", "replace"))

        sheet = read("assets/GJ_GameSheet.plist")
        add("")
        add("## GJ_GameSheet frames")
        if sheet is None:
            add("missing")
        else:
            try:
                d = plistlib.loads(sheet)
                frames = sorted((d.get("frames") or {}).keys())
                add(f"frame_count {len(frames)}")
                add("portal_frames " + ",".join(f for f in frames if "portal" in f))
                add("mirror_frames " + ",".join(f for f in frames if "mirror" in f.lower()))
                add("player_frames " + ",".join(f for f in frames if f.startswith("player_")))
                add("all_frames " + ",".join(frames))
            except Exception as exc:  # noqa: BLE001
                add(f"unparsable: {exc!r}")

    lib = None
    with zipfile.ZipFile(apk) as zf:
        for cand in ("lib/armeabi/libgame.so",):
            try:
                lib = zf.read(cand)
            except KeyError:
                continue
    if lib is not None:
        add("")
        add(f"## libgame.so size={len(lib)}")
        text = "\n".join(_strings(lib))
        add("## string survey")
        for label, pat in STRING_PATTERNS:
            add(f"{label} {len(re.findall(pat, text))}")
        add("")
        add("## portal strings")
        for s in sorted({s for s in _strings(lib) if "portal" in s.lower()}):
            add(s)
        add("")
        add("## level-name strings")
        for s in sorted({s for s in _strings(lib) if re.search(LEVEL_NAMES, s, re.I)}):
            add(s)
        add("")
        syms = _elf_symbols(lib)
        add(f"## symbols total={len(syms)}")
        interesting = [s for s in syms if re.search(r"PlayLayer|GameObject|Portal|LevelTools|GameLevelManager|ObjectToolbox|CCLabel|CCTexture|GameToolbox", s)]
        add(f"interesting={len(interesting)}")
        for s in sorted(set(interesting)):
            add(s)

    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} bytes)")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("apk", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    dump(args.apk, args.out)


if __name__ == "__main__":
    main()
