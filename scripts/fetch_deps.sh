#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Fetches pinned third-party build dependencies into third_party/ (gitignored):
#   - azahar-emu/dynarmic (0BSD, pinned commit + ARM64 fast-dispatch patch)
#   - madler/zlib v1.3.1 (Zlib license, compiled into ARM32 libguestrt.so)
#   - Boost header-only libraries (BSL-1.0, used by Dynarmic)
#   - Apktool v2.11.1 (Apache-2.0, APK decode/rebuild)
#   - Original Geometry.Dash.ver.1.02.build.4.apk (optional, --with-apk, SHA-256 verified)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

DYNARMIC_URL="https://github.com/azahar-emu/dynarmic.git"
DYNARMIC_REV="a46601580d5512d324104f985b5f0209dc980ddc"

ZLIB_URL="https://github.com/madler/zlib.git"
ZLIB_TAG="v1.3.1"

BOOST_URL="https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2"
BOOST_SHA256="1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b"

APKTOOL_URL="https://github.com/iBotPeaches/Apktool/releases/download/v2.11.1/apktool_2.11.1.jar"
APKTOOL_SHA256="56d59c524fc764263ba8d345754d8daf55b1887818b15cd3b594f555d249e2db"

ORIG_APK_SHA256="37ae0e63820572994e63421bb3b138731ac40f9e6bb1030e137e9eaf03aa68a9"

# Geometry Dash 1.1 build 5, the release that introduced Time Machine and mirror portals.
# SHA-256 recorded from a verified CI fetch of the archive.org copy.
GD11_APK_SHA256="${GD11_APK_SHA256:-854f73b21bb555edc206bd89d024c931d3eca630a1e32c7c2c191182e8eaed20}"
GD11_APK_SIZE=19482603
GD11_APK_URLS=(
    "https://ia803202.us.archive.org/view_archive.php?archive=/22/items/every-geometry-dash-version/every%20geometry%20dash%20version.zip&file=Geometry.Dash.ver.1.1.build.5.apk"
    "https://ia903202.us.archive.org/view_archive.php?archive=/22/items/every-geometry-dash-version/every%20geometry%20dash%20version.zip&file=Geometry.Dash.ver.1.1.build.5.apk"
    "https://archive.org/download/every-geometry-dash-version/every%20geometry%20dash%20version.zip/Geometry.Dash.ver.1.1.build.5.apk"
)
ORIG_APK_URLS=(
    "https://ia803202.us.archive.org/view_archive.php?archive=/22/items/every-geometry-dash-version/every%20geometry%20dash%20version.zip&file=Geometry.Dash.ver.1.02.build.4.apk"
    "https://ia903202.us.archive.org/view_archive.php?archive=/22/items/every-geometry-dash-version/every%20geometry%20dash%20version.zip&file=Geometry.Dash.ver.1.02.build.4.apk"
    "https://archive.org/download/every-geometry-dash-version/every%20geometry%20dash%20version.zip/Geometry.Dash.ver.1.02.build.4.apk"
)

verify_sha256() {
    echo "$2  $1" | sha256sum -c --quiet - || {
        echo "ERROR: SHA-256 mismatch for $1" >&2
        exit 1
    }
}

# 1. Dynarmic + ARM64 fast-dispatch patch
if [ ! -d "$TP/dynarmic/.git" ]; then
    echo "==> Cloning azahar-emu/dynarmic ($DYNARMIC_REV)"
    git clone --quiet "$DYNARMIC_URL" "$TP/dynarmic"
    git -C "$TP/dynarmic" checkout --quiet "$DYNARMIC_REV"
    git -C "$TP/dynarmic" submodule update --quiet --init --recursive --depth 1
fi
for patch in "$ROOT"/patches/dynarmic-*.patch; do
    [ -e "$patch" ] || continue
    if git -C "$TP/dynarmic" apply --reverse --check "$patch" 2>/dev/null; then
        continue
    fi
    echo "==> Applying $(basename "$patch")"
    git -C "$TP/dynarmic" apply "$patch"
done

# 2. zlib v1.3.1 (for ARM32 guest runtime)
if [ ! -f "$TP/zlib/zlib.h" ]; then
    echo "==> Cloning madler/zlib ($ZLIB_TAG)"
    rm -rf "$TP/zlib"
    git clone --quiet --depth 1 --branch "$ZLIB_TAG" "$ZLIB_URL" "$TP/zlib"
fi

# 3. Boost headers (use system /usr/include/boost if installed, otherwise download Boost 1.86.0 headers)
if [ ! -d "$TP/boost/boost" ]; then
    if [ -f "/usr/include/boost/icl/interval_map.hpp" ]; then
        echo "==> Linking system Boost headers (/usr/include/boost)"
        mkdir -p "$TP/boost"
        ln -s /usr/include/boost "$TP/boost/boost"
    else
        echo "==> Downloading Boost 1.86.0 headers"
        tmp="$(mktemp -d)"
        curl -fL --retry 3 --connect-timeout 20 -o "$tmp/boost.tar.bz2" "$BOOST_URL"
        verify_sha256 "$tmp/boost.tar.bz2" "$BOOST_SHA256"
        mkdir -p "$TP/boost"
        tar -xjf "$tmp/boost.tar.bz2" -C "$TP/boost" --strip-components=1 boost_1_86_0/boost boost_1_86_0/LICENSE_1_0.txt
        rm -rf "$tmp"
    fi
fi

# 4. Apktool v2.11.1
if [ ! -f "$TP/apktool.jar" ]; then
    echo "==> Downloading Apktool v2.11.1"
    curl -fL --retry 3 --connect-timeout 20 -o "$TP/apktool.jar.part" "$APKTOOL_URL"
    verify_sha256 "$TP/apktool.jar.part" "$APKTOOL_SHA256"
    mv "$TP/apktool.jar.part" "$TP/apktool.jar"
fi

# 5. Optional: fetch unmodified Geometry.Dash.ver.1.02.build.4.apk from Internet Archive
if [ "${1:-}" = "--with-apk" ]; then
    APK_DEST="${2:-$TP/Geometry.Dash.ver.1.02.build.4.apk}"
    if [ -f "$APK_DEST" ] && echo "$ORIG_APK_SHA256  $APK_DEST" | sha256sum -c --quiet - 2>/dev/null; then
        echo "==> Verified cached original APK at $APK_DEST"
    else
        echo "==> Downloading original Geometry.Dash.ver.1.02.build.4.apk"
        mkdir -p "$(dirname "$APK_DEST")"
        rm -f "$APK_DEST.part"
        for url in "${ORIG_APK_URLS[@]}"; do
            if curl -fL --connect-timeout 20 --max-time 180 -o "$APK_DEST.part" "$url"; then
                if echo "$ORIG_APK_SHA256  $APK_DEST.part" | sha256sum -c --quiet - 2>/dev/null; then
                    mv "$APK_DEST.part" "$APK_DEST"
                    echo "==> Verified SHA-256 of $APK_DEST"
                    break
                fi
            fi
            rm -f "$APK_DEST.part"
        done
        if [ ! -f "$APK_DEST" ]; then
            echo "ERROR: Failed to download Geometry.Dash.ver.1.02.build.4.apk" >&2
            exit 1
        fi
    fi
fi

# 6. Optional: fetch unmodified Geometry.Dash.ver.1.1.build.5.apk (mirror portals + Time Machine)
if [ "${1:-}" = "--with-apk11" ]; then
    APK_DEST="${2:-$TP/Geometry.Dash.ver.1.1.build.5.apk}"
    ok=0
    if [ -f "$APK_DEST" ]; then
        if [ -n "$GD11_APK_SHA256" ]; then
            echo "$GD11_APK_SHA256  $APK_DEST" | sha256sum -c --quiet - 2>/dev/null && ok=1
        elif [ "$(stat -c%s "$APK_DEST")" = "$GD11_APK_SIZE" ]; then
            ok=1
        fi
    fi
    if [ "$ok" = "1" ]; then
        echo "==> Verified cached GD 1.1 APK at $APK_DEST"
    else
        echo "==> Downloading original Geometry.Dash.ver.1.1.build.5.apk"
        mkdir -p "$(dirname "$APK_DEST")"
        rm -f "$APK_DEST.part"
        for url in "${GD11_APK_URLS[@]}"; do
            if curl -fL --connect-timeout 20 --max-time 300 -o "$APK_DEST.part" "$url"; then
                if [ -n "$GD11_APK_SHA256" ]; then
                    if echo "$GD11_APK_SHA256  $APK_DEST.part" | sha256sum -c --quiet - 2>/dev/null; then
                        mv "$APK_DEST.part" "$APK_DEST"; break
                    fi
                elif [ "$(stat -c%s "$APK_DEST.part")" = "$GD11_APK_SIZE" ]; then
                    mv "$APK_DEST.part" "$APK_DEST"; break
                fi
            fi
            rm -f "$APK_DEST.part"
        done
        if [ ! -f "$APK_DEST" ]; then
            echo "ERROR: Failed to download Geometry.Dash.ver.1.1.build.5.apk" >&2
            exit 1
        fi
        echo "==> GD 1.1 APK sha256: $(sha256sum "$APK_DEST" | cut -d' ' -f1)"
    fi
fi

echo "==> Dependencies ready in $TP"
