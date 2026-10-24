#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# One-shot reverse-engineering helper: dumps the symbol and disassembly facts required to wire the
# PC editor keybinds (shift->swipe, wheel->zoom, ...) against the real ARM32 guest binaries.
# Runs in CI (ubuntu binutils understands ARM), writes tools/analysis/guest_keybind_facts.txt.
#
# Facts gathered per guest (1.02 and 1.1):
#   * presence/value of the cocos2d scene-graph getters used for editor detection
#   * EditorUI / LevelEditorLayer full dynamic symbol lists
#   * first words of the relevant vtables (determines the object-vptr delta)
#   * EditorUI::toggleSwipe disassembly  -> the swipe flag's byte offset inside EditorUI
#   * LevelEditorLayer::init context around `bl EditorUI::create` -> EditorUI* field offset
#   * EditorUI::init strb offsets        -> cross-check of the swipe flag init
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/tools/analysis/guest_keybind_facts.txt"
APK102="${GD102_APK:-$ROOT/third_party/Geometry.Dash.ver.1.02.build.4.apk}"
APK11="${GD11_APK:-$ROOT/third_party/Geometry.Dash.ver.1.1.build.5.apk}"

OBJDUMP="${OBJDUMP:-arm-linux-gnueabi-objdump}"
NM="${NM:-nm}"

command -v "$OBJDUMP" >/dev/null || { echo "need $OBJDUMP"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

disasm_fn() { # $1=disfile $2=symbol
    awk -v s="$2" '
        index($0, " <" s ">:") { f=1; print; next }
        f && /^[0-9a-f]+ </ { exit }
        f { print }
    ' "$1"
}

dump_one() { # $1=label $2=apk
    local label="$1" apk="$2"
    local dir="$TMP/$1"
    echo "### guest $label"
    if [ ! -f "$apk" ]; then
        echo "MISSING APK: $apk"
        return 0
    fi
    mkdir -p "$dir"
    if ! unzip -o -q "$apk" "lib/armeabi/libgame.so" -d "$dir"; then
        echo "no lib/armeabi/libgame.so in $apk"
        return 0
    fi
    local so="$dir/lib/armeabi/libgame.so"
    echo "so_size $(stat -c%s "$so") so_sha256 $(sha256sum "$so" | cut -d' ' -f1)"

    "$NM" -D --defined-only "$so" > "$dir/dyn.txt" 2>/dev/null || {
        echo "nm failed on $so"; return 0;
    }
    echo "export_count $(wc -l < "$dir/dyn.txt")"

    echo "-- scene-graph / vtable / editor symbols (value + type) --"
    grep -E "(_ZN7cocos2d10CCDirector1[45](sharedDirector|getRunningScene)Ev|_ZN7cocos2d6CCNode11getChildrenEv|_ZNK?7cocos2d7CCArray(5count|13objectAtIndex)E?v?|_ZN7cocos2d6CCNode1[45]getPosition[XY]Ev|_ZN7cocos2d6CCNode11setPositionEff|_ZTV(16LevelEditorLayer|8EditorUI|9PlayLayer|7CCScene)|_ZTI(S|16LevelEditorLayer|8EditorUI)|_ZTS(16LevelEditorLayer|8EditorUI)|toggleSwipe|zoomIn|zoomOut|undoLastAction|redoLastAction|getGameLayer|constrainGameLayerPosition|copyObject|pasteObject)" "$dir/dyn.txt" || true

    echo "-- CCDirector symbols (scene access is inline; drawScene/mainLoop reveal field offsets) --"
    grep -E "_ZN7cocos2d10CCDirector[0-9]+" "$dir/dyn.txt" | head -60 || true

    echo "-- CCDirector::drawScene / mainLoop / getWinSize disasm (m_pRunningScene / m_obWinSizeInPoints offsets) --"
    for sym in _ZN7cocos2d10CCDirector8drawSceneEv _ZN7cocos2d10CCDirector8mainLoopEv _ZN7cocos2d10CCDirector10getWinSizeEv _ZN7cocos2d10CCDirector17calculateDeltaTimeEv; do
        out="$(disasm_fn "$dir/dis.txt" "$sym" | head -70)"
        if [ -n "$out" ]; then echo "symbol: $sym"; echo "$out"; fi
    done

    echo "-- CCNode position accessors (needed for arrow-key pan) --"
    grep -E "_ZNK?7cocos2d6CCNode[0-9]+getPosition" "$dir/dyn.txt" || true

    echo "-- all EditorUI / LevelEditorLayer dynamic symbols --"
    grep -E "_ZN(8EditorUI|16LevelEditorLayer)" "$dir/dyn.txt" || true

    echo "-- all dynsyms containing 'Editor' (catches globals/vtables) --"
    grep -i "editor" "$dir/dyn.txt" | grep -vE "_ZN(8EditorUI|16LevelEditorLayer)" || true

    echo "-- candidate copy/paste/duplicate APIs --"
    grep -iE "paste|duplicate|copyobject|copy_objects" "$dir/dyn.txt" || true

    # Full disassembly for context greps (kept in temp only).
    "$OBJDUMP" -d "$so" > "$dir/dis.txt"

    echo "-- EditorUI::toggleSwipe (full disassembly; strb/ldrb [rX,#imm] reveals the flag offset) --"
    for sym in _ZN8EditorUI11toggleSwipeEv _ZN8EditorUI12toggleSwipeEv; do
        out="$(disasm_fn "$dir/dis.txt" "$sym")"
        if [ -n "$out" ]; then echo "symbol: $sym"; echo "$out"; fi
    done

    echo "-- EditorUI::zoomIn / zoomOut / undoLastAction / redoLastAction (heads, 40 lines) --"
    for sym in _ZN8EditorUI6zoomInEv _ZN8EditorUI7zoomOutEv _ZN8EditorUI12undoLastActionEv _ZN8EditorUI12redoLastActionEv _ZN8EditorUI11undoLastActionEv _ZN8EditorUI11redoLastActionEv; do
        out="$(disasm_fn "$dir/dis.txt" "$sym" | head -40)"
        if [ -n "$out" ]; then echo "symbol: $sym"; echo "$out"; fi
    done

    echo "-- callers of EditorUI::create (context; the str after the bl shows the LEL field offset) --"
    grep -n -B12 -A16 "bl.*<_ZN8EditorUI6createEP16LevelEditorLayer>" "$dir/dis.txt" | head -240 || true

    echo "-- callers of toggleSwipe (button wiring) --"
    grep -n -B8 -A2 "bl.*<_ZN8EditorUI1[12]toggleSwipeEv>" "$dir/dis.txt" | head -80 || true

    echo "-- EditorUI::init: all stores (candidate flag init; swipe flag lives at #348/0x15c) --"
    disasm_fn "$dir/dis.txt" _ZN8EditorUI4initEP16LevelEditorLayer | grep -E "str|#348|0x15c" | head -120 || true

    echo "-- any instruction referencing the swipe flag offset (#348 / 0x15c) inside EditorUI functions --"
    grep -nE "#348|@ 0x15c" "$dir/dis.txt" | head -40 || true

    echo "-- LevelEditorLayer::init: stores after EditorUI-touching calls (first 120 lines) --"
    disasm_fn "$dir/dis.txt" _ZN16LevelEditorLayer4initEP11GJGameLevel | grep -n -E "bl|str " | head -120 || true

    echo "-- vtable heads (object vptr = symbol + 8 or +16 depending on slot 1) --"
    grep -E "_ZTV(16LevelEditorLayer|8EditorUI|9PlayLayer|7CCScene)([ ]|$)" "$dir/dyn.txt" | while read -r a t s; do
        [ "$t" = "V" ] || [ "$t" = "D" ] || [ "$t" = "d" ] || [ "$t" = "R" ] || true
        python3 - "$so" "$a" "$s" <<'PYEOF'
import struct, sys
path, addr, name = sys.argv[1], int(sys.argv[2], 16), sys.argv[3]
d = open(path, 'rb').read()
e_phoff, = struct.unpack_from('<I', d, 28)
e_phentsize, e_phnum = struct.unpack_from('<HH', d, 42)
ph = [struct.unpack_from('<IIIIIIII', d, e_phoff + i * e_phentsize) for i in range(e_phnum)]
off = None
for p_type, p_off, p_vaddr, _p_paddr, p_filesz, _p_memsz, _p_flags, _p_align in ph:
    if p_type == 1 and p_vaddr <= addr < p_vaddr + p_filesz:
        off = p_off + (addr - p_vaddr)
        break
if off is None:
    print(f'{name} vaddr=0x{addr:x} UNMAPPED')
else:
    words = [f'0x{struct.unpack_from("<I", d, off + i * 4)[0]:08x}' for i in range(8)]
    print(f'{name} vaddr=0x{addr:x} first8= ' + ' '.join(words))
PYEOF
    done

    echo "-- EditorUI::create / LevelEditorLayer ctor-adjacent symbol presence (sizes via nm) --"
    grep -E "_ZN8EditorUI6(create|4init)EP16LevelEditorLayer" "$dir/dyn.txt" || true
    echo
}

{
    echo "# Guest keybind facts (generated by scripts/inspect_guest_keybinds.sh in CI)"
    echo "# date_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo
    dump_one "gd102" "$APK102"
    dump_one "gd11" "$APK11"
} | tee "$OUT"

echo "==> wrote $OUT"
