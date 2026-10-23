#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Comprehensive structural, ELF64 16 KB page-alignment, zip-offset alignment, symbol,
and asset integrity verifier for the rebuilt Geometry Dash 1.02 ARM64 APK.
"""

import hashlib
import struct
import sys
import zipfile

ORIG_LIBGAME_SHA256 = "787b44036ee6f155168b7879f8fab1b733826a0442dd3eec70b157e7e75979db"

REQUIRED_EXPORTS = [
    "JNI_OnLoad",
    "Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeBridgeInit",
    "Java_com_robtopx_geometryjump_ModernPlatformHelper_nativeOnSurfaceCreated",
    "Java_com_customRobTop_JniToCpp_didCacheInterstitial",
    "Java_com_customRobTop_JniToCpp_didClickInterstitial",
    "Java_com_customRobTop_JniToCpp_didCloseInterstitial",
    "Java_com_customRobTop_JniToCpp_didDismissInterstitial",
    "Java_com_customRobTop_JniToCpp_hideLoadingCircle",
    "Java_com_customRobTop_JniToCpp_itemPurchaseFailed",
    "Java_com_customRobTop_JniToCpp_itemPurchased",
    "Java_com_customRobTop_JniToCpp_itemRefunded",
    "Java_com_customRobTop_JniToCpp_promoImageDownloaded",
    "Java_com_customRobTop_JniToCpp_resumeSound",
    "Java_com_customRobTop_JniToCpp_showInterstitialFailed",
    "Java_org_cocos2dx_lib_Cocos2dxAccelerometer_onSensorChanged",
    "Java_org_cocos2dx_lib_Cocos2dxActivity_nativeSetPaths",
    "Java_org_cocos2dx_lib_Cocos2dxBitmap_nativeInitBitmapDC",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeDeleteBackward",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeGetContentText",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInsertText",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd",
    "Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove",
]


def zip_entry_data_offset(raw_apk: bytes, info: zipfile.ZipInfo) -> int:
    off = info.header_offset
    sig, _, _, _, _, _, _, _, _, fname_len, extra_len = struct.unpack_from("<4sHHHHHIIIHH", raw_apk, off)
    if sig != b"PK\x03\x04":
        raise RuntimeError(f"Bad local file header signature for {info.filename}")
    return off + 30 + fname_len + extra_len


def parse_elf64_exports_and_align(elf: bytes) -> tuple[int, list[int], set[str]]:
    if elf[:4] != b"\x7fELF" or elf[4] != 2 or elf[5] != 1:
        raise RuntimeError("lib/arm64-v8a/libgame.so is not little-endian ELF64")
    e_machine = struct.unpack_from("<H", elf, 18)[0]
    e_phoff = struct.unpack_from("<Q", elf, 32)[0]
    e_shoff = struct.unpack_from("<Q", elf, 40)[0]
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from("<HHHH", elf, 54)

    load_aligns = []
    for i in range(e_phnum):
        poff = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(
            "<IIQQQQQQ", elf, poff
        )
        if p_type == 1:  # PT_LOAD
            load_aligns.append(p_align)

    exports: set[str] = set()
    for i in range(e_shnum):
        soff = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = (
            struct.unpack_from("<IIQQQQIIQQ", elf, soff)
        )
        if sh_type == 11:  # SHT_DYNSYM
            str_soff = e_shoff + sh_link * e_shentsize
            _, _, _, _, str_off, str_sz, _, _, _, _ = struct.unpack_from("<IIQQQQIIQQ", elf, str_soff)
            strtab = elf[str_off : str_off + str_sz]
            nsyms = sh_size // (sh_entsize or 24)
            for s in range(nsyms):
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                    "<IBBHQQ", elf, sh_offset + s * 24
                )
                if st_shndx != 0 and st_value != 0:
                    end = strtab.find(b"\x00", st_name)
                    if end > st_name:
                        exports.add(strtab[st_name:end].decode("utf-8", errors="replace"))
    return e_machine, load_aligns, exports


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <path_to_apk>", file=sys.stderr)
        sys.exit(2)

    apk_path = sys.argv[1]
    with open(apk_path, "rb") as f:
        raw_apk = f.read()

    with zipfile.ZipFile(apk_path, "r") as zf:
        infos = {i.filename: i for i in zf.infolist()}

        # 1. Native library ABI check: ONLY arm64-v8a/libgame.so
        lib_entries = [k for k in infos if k.startswith("lib/")]
        if lib_entries != ["lib/arm64-v8a/libgame.so"]:
            raise RuntimeError(f"Expected only ['lib/arm64-v8a/libgame.so'], got {lib_entries}")

        host_info = infos["lib/arm64-v8a/libgame.so"]
        if host_info.compress_type != zipfile.ZIP_STORED:
            raise RuntimeError("lib/arm64-v8a/libgame.so MUST be STORED uncompressed for extractNativeLibs=false")
        host_off = zip_entry_data_offset(raw_apk, host_info)
        if host_off % 16384 != 0:
            raise RuntimeError(f"lib/arm64-v8a/libgame.so zip offset 0x{host_off:x} is NOT 16 KiB aligned")

        host_elf = zf.read("lib/arm64-v8a/libgame.so")
        e_machine, load_aligns, exports = parse_elf64_exports_and_align(host_elf)
        if e_machine != 183:  # EM_AARCH64
            raise RuntimeError(f"Expected EM_AARCH64 (183), got {e_machine}")
        if not load_aligns or any(a < 16384 for a in load_aligns):
            raise RuntimeError(f"PT_LOAD p_align < 16384 in lib/arm64-v8a/libgame.so: {load_aligns}")

        missing_exports = [s for s in REQUIRED_EXPORTS if s not in exports]
        if missing_exports:
            raise RuntimeError(f"Missing required JNI exports in ARM64 libgame.so: {missing_exports}")

        # 2. DEX verification (classes.dex + classes2.dex)
        for dex in ("classes.dex", "classes2.dex"):
            if dex not in infos:
                raise RuntimeError(f"Missing {dex} in APK")
        dex2 = zf.read("classes2.dex")
        if b"Lcom/robtopx/geometryjump/ModernPlatformHelper;" not in dex2:
            raise RuntimeError("ModernPlatformHelper class missing from classes2.dex")

        # 3. Embedded ARM32 guest runtime + unmodified original GD 1.02 libgame.so
        for guest_so in ("assets/arm32/libguestrt.so", "assets/arm32/libgame.so"):
            if guest_so not in infos:
                raise RuntimeError(f"Missing {guest_so}")
            if infos[guest_so].compress_type != zipfile.ZIP_STORED:
                raise RuntimeError(f"{guest_so} must be STORED uncompressed for direct mmap/AAsset_getBuffer")

        guest_game = zf.read("assets/arm32/libgame.so")
        guest_sha = hashlib.sha256(guest_game).hexdigest()
        if guest_sha != ORIG_LIBGAME_SHA256:
            raise RuntimeError(
                f"assets/arm32/libgame.so SHA-256 mismatch: got {guest_sha}, expected {ORIG_LIBGAME_SHA256}"
            )

        # 4. Original game assets + STORED audio/resources check
        orig_assets = [
            k for k in infos if k.startswith("assets/") and not k.startswith("assets/arm32/") and not k.endswith("/")
        ]
        if len(orig_assets) != 136:
            raise RuntimeError(f"Expected all 136 original GD 1.02 assets, found {len(orig_assets)}")

        for name, info in infos.items():
            if name == "resources.arsc" or name.endswith(".mp3") or name.endswith(".ogg"):
                if info.compress_type != zipfile.ZIP_STORED:
                    raise RuntimeError(f"{name} must be STORED uncompressed (got compress_type={info.compress_type})")
                off = zip_entry_data_offset(raw_apk, info)
                if name == "resources.arsc" and off % 4 != 0:
                    raise RuntimeError(f"resources.arsc offset 0x{off:x} is not 4-byte aligned")

    print("APK VERIFICATION PASSED:")
    print(f"  - ABI: arm64-v8a ONLY (EM_AARCH64, PT_LOAD aligns={[hex(x) for x in load_aligns]})")
    print(f"  - Zip 16 KB alignment: lib/arm64-v8a/libgame.so @ 0x{host_off:x} (mod 16384 == 0)")
    print(f"  - Exported JNI symbols verified: {len(REQUIRED_EXPORTS)}/{len(REQUIRED_EXPORTS)}")
    print(f"  - Guest ARM32 libgame.so SHA-256: {guest_sha} (100% unmodified build 4)")
    print(f"  - Original game assets verified: {len(orig_assets)}/136 (audio & resources.arsc STORED)")


if __name__ == "__main__":
    main()
