#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds libguestrt.so: the 32-bit ARM guest runtime (allocator, pure libc/string/stdio,
# setjmp/longjmp, compiler-rt __aeabi_* helpers, and zlib 1.3.1) loaded into the guest
# address space alongside the unmodified Geometry Dash 1.02 libgame.so.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
ARG="${1:-$ROOT/out/libguestrt.so}"

if [[ "$ARG" == *.so ]]; then
    OUT_SO="$ARG"
    mkdir -p "$(dirname "$OUT_SO")"
else
    mkdir -p "$ARG"
    OUT_SO="$ARG/libguestrt.so"
fi

TMP_BUILD="$(mktemp -d)"
trap 'rm -rf "$TMP_BUILD"' EXIT

ZLIB_DIR="$ROOT/third_party/zlib"
if [ ! -d "$ZLIB_DIR" ]; then
    echo "error: $ZLIB_DIR not found; run scripts/fetch_deps.sh first" >&2
    exit 1
fi

AEABI="__aeabi_idiv __aeabi_idivmod __aeabi_uidiv __aeabi_uidivmod __aeabi_ldivmod __aeabi_uldivmod
       __aeabi_d2lz __aeabi_d2ulz __aeabi_f2lz __aeabi_f2ulz __aeabi_l2d __aeabi_l2f __aeabi_ul2d __aeabi_ul2f
       __aeabi_memcpy __aeabi_memcpy4 __aeabi_memcpy8 __aeabi_memmove __aeabi_memmove4 __aeabi_memmove8
       __aeabi_memset __aeabi_memset4 __aeabi_memset8 __aeabi_memclr __aeabi_memclr4 __aeabi_memclr8"
UNDEF=$(for s in $AEABI; do printf -- "-Wl,-u,%s " "$s"; done)

# Promote STV_HIDDEN -> STV_DEFAULT on the target __aeabi_* symbols inside an ELF32 .a archive
# without depending on toolchain-version-specific objcopy CLI flags.
promote_archive_visibility() {
    python3 - "$1" "$2" $AEABI << 'PYEOF'
import struct, sys

src_path, dst_path = sys.argv[1], sys.argv[2]
targets = {s.encode("utf-8") for s in sys.argv[3:]}

with open(src_path, "rb") as f:
    ar = bytearray(f.read())

if ar[:8] != b"!<arch>\n":
    raise RuntimeError(f"{src_path} is not a standard !<arch> static library")

pos = 8
while pos + 60 <= len(ar):
    sz = int(ar[pos + 48 : pos + 58].decode("ascii").strip())
    dstart = pos + 60
    dend = dstart + sz
    if ar[dstart : dstart + 5] == b"\x7fELF\x01":
        e_shoff = struct.unpack_from("<I", ar, dstart + 32)[0]
        e_shentsize, e_shnum = struct.unpack_from("<HH", ar, dstart + 46)
        for i in range(e_shnum):
            soff = dstart + e_shoff + i * e_shentsize
            sh_type, _, _, sh_offset, sh_size, sh_link, _, _, sh_entsize = struct.unpack_from("<IIIIIIIII", ar, soff + 4)
            if sh_type == 2:  # SHT_SYMTAB
                str_soff = dstart + e_shoff + sh_link * e_shentsize
                str_off, str_sz = struct.unpack_from("<II", ar, str_soff + 16)
                stab = bytes(ar[dstart + str_off : dstart + str_off + str_sz])
                ent = sh_entsize or 16
                for s in range(sh_size // ent):
                    sym_p = dstart + sh_offset + s * ent
                    st_name = struct.unpack_from("<I", ar, sym_p)[0]
                    end = stab.find(b"\x00", st_name)
                    if end > st_name and stab[st_name:end] in targets:
                        ar[sym_p + 13] &= ~0x03  # STV_DEFAULT
    pos = dend + (sz & 1)

with open(dst_path, "wb") as f:
    f.write(ar)
PYEOF
}

ZLIB_SRCS=(
    "$ZLIB_DIR/adler32.c"
    "$ZLIB_DIR/compress.c"
    "$ZLIB_DIR/crc32.c"
    "$ZLIB_DIR/deflate.c"
    "$ZLIB_DIR/gzclose.c"
    "$ZLIB_DIR/gzlib.c"
    "$ZLIB_DIR/gzread.c"
    "$ZLIB_DIR/gzwrite.c"
    "$ZLIB_DIR/infback.c"
    "$ZLIB_DIR/inffast.c"
    "$ZLIB_DIR/inflate.c"
    "$ZLIB_DIR/inftrees.c"
    "$ZLIB_DIR/trees.c"
    "$ZLIB_DIR/uncompr.c"
    "$ZLIB_DIR/zutil.c"
)

COMMON_DEFS=(
    -O2 -marm -fPIC -ffreestanding -fno-builtin -fno-stack-protector
    -fvisibility=hidden -D_FILE_OFFSET_BITS=32
    -DHAVE_UNISTD_H=1 -DHAVE_STDARG_H=1
    '-DZEXTERN=extern __attribute__((visibility("default")))'
)

if [ -n "${NDK:-}" ] && [ -d "${NDK:-}" ]; then
    TC="${NDK_TC:-$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/ | head -n 1)}"
    CC="$(ls -1 "$TC"/bin/armv7a-linux-androideabi*-clang | sort -V | head -n 1)"
    RT_ARCHIVE="$("$CC" -rtlib=compiler-rt -print-libgcc-file-name)"
    BUILTINS="$TMP_BUILD/libbuiltins-export.a"
    promote_archive_visibility "$RT_ARCHIVE" "$BUILTINS"
    "$CC" "${COMMON_DEFS[@]}" -mfloat-abi=softfp -mfpu=vfpv3-d16 -Wno-everything \
        -shared -nostdlib -Wl,-soname,libguestrt.so -Wl,-Bsymbolic -Wl,--no-undefined-version \
        -Wl,--hash-style=both -Wl,-z,norelro \
        "$HERE/rt_string.c" "$HERE/rt_stdio.c" "$HERE/rt_malloc.c" "$HERE/rt_asm.S" \
        "${ZLIB_SRCS[@]}" \
        $UNDEF "$BUILTINS" \
        -o "$OUT_SO"
elif command -v arm-linux-gnueabi-gcc >/dev/null 2>&1; then
    LIBGCC="$(arm-linux-gnueabi-gcc -march=armv7-a -marm -mfloat-abi=soft -print-libgcc-file-name)"
    BUILTINS="$TMP_BUILD/libgcc-export.a"
    promote_archive_visibility "$LIBGCC" "$BUILTINS"
    arm-linux-gnueabi-gcc "${COMMON_DEFS[@]}" -march=armv7-a -mfloat-abi=soft -w \
        -shared -nostdlib -Wl,-soname,libguestrt.so -Wl,-Bsymbolic \
        -Wl,--hash-style=both -Wl,-z,norelro \
        "$HERE/rt_string.c" "$HERE/rt_stdio.c" "$HERE/rt_malloc.c" "$HERE/rt_asm.S" \
        "${ZLIB_SRCS[@]}" \
        $UNDEF "$BUILTINS" \
        -o "$OUT_SO"
else
    echo "error: set NDK=/path/to/android-ndk or install arm-linux-gnueabi-gcc" >&2
    exit 1
fi

chmod +x "$OUT_SO"
echo "Built $OUT_SO ($(stat -c%s "$OUT_SO") bytes)"
