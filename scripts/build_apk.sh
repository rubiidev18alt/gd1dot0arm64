#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# End-to-end Android 16 (API 36) ARM64 APK builder for Geometry Dash 1.02 (build 4).
# Usage:
#   ./scripts/build_apk.sh [path/to/Geometry.Dash.ver.1.02.build.4.apk] [output.apk]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Input APK: defaults to Geometry Dash 1.02 build 4, but any compatible RobTop
# build can be supplied (the 1.1 variant uses GD 1.1 build 5). The bridge itself is
# version-agnostic; only the smali patches and the embedded guest binary change.
GD_VERSION="${GD102_VERSION:-1.02}"
ORIG_APK="${GD102_INPUT_APK:-${1:-$ROOT/third_party/Geometry.Dash.ver.1.02.build.4.apk}}"
# An explicitly empty GD102_INPUT_SHA256 means "this build is pinned elsewhere" (the 1.1
# fetch in fetch_deps.sh pins by SHA/size), so honour it instead of falling back.
if [ "${GD102_INPUT_SHA256+set}" = "set" ]; then
    EXPECTED_INPUT_SHA256="$GD102_INPUT_SHA256"
else
    EXPECTED_INPUT_SHA256="37ae0e63820572994e63421bb3b138731ac40f9e6bb1030e137e9eaf03aa68a9"
fi
PACKAGE_NAME="${GD102_PACKAGE_NAME:-com.robtopx.geometryj102}"
APP_NAME="${GD102_APP_NAME:-gd 1.02 arm64}"
OUT_APK="${2:-$ROOT/dist/gd-${GD_VERSION}-arm64-${PACKAGE_NAME}.apk}"

# Optional: embed the private local 1.0 GDPS level archive (scripts/build_gdhistory_db.py output).
# Set GD102_LOCAL_GDPS=1 (and GD102_GDPS_DB=/path/levels_1_0.bin) to build the offline-server variant.
LOCAL_GDPS="${GD102_LOCAL_GDPS:-0}"
GDPS_DB="${GD102_GDPS_DB:-$ROOT/third_party/levels_1_0.bin}"
if [ "$LOCAL_GDPS" = "1" ] && [ ! -f "$GDPS_DB" ]; then
    echo "ERROR: GD102_LOCAL_GDPS=1 but level archive '$GDPS_DB' is missing." >&2
    echo "       Run: python3 scripts/build_gdhistory_db.py --out third_party/levels_1_0.bin --orig-apk \"$ORIG_APK\"" >&2
    exit 1
fi

# 1. Discover Android SDK, NDK, Build-Tools (35+/36+ for zipalign -P 16), and android.jar (API 36 preferred)
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}"
if [ -z "$SDK" ] || [ ! -d "$SDK" ]; then
    echo "ERROR: Set ANDROID_SDK_ROOT or ANDROID_HOME to your Android SDK path." >&2
    exit 1
fi

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    ANDROID_NDK_HOME="$(ls -1d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [ -z "$ANDROID_NDK_HOME" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "ERROR: Android NDK not found under $SDK/ndk; set ANDROID_NDK_HOME." >&2
    exit 1
fi
export NDK="$ANDROID_NDK_HOME"

BUILD_TOOLS="$(ls -1d "$SDK"/build-tools/* 2>/dev/null | sort -V | tail -n 1 || true)"
if [ -z "$BUILD_TOOLS" ] || [ ! -x "$BUILD_TOOLS/zipalign" ]; then
    echo "ERROR: Android build-tools not found under $SDK/build-tools." >&2
    exit 1
fi

ANDROID_JAR=""
for api in 36 35 34; do
    if [ -f "$SDK/platforms/android-$api/android.jar" ]; then
        ANDROID_JAR="$SDK/platforms/android-$api/android.jar"
        break
    fi
done
if [ -z "$ANDROID_JAR" ]; then
    ANDROID_JAR="$(ls -1 "$SDK"/platforms/android-*/android.jar 2>/dev/null | sort -V | tail -n 1 || true)"
fi
if [ -z "$ANDROID_JAR" ] || [ ! -f "$ANDROID_JAR" ]; then
    echo "ERROR: android.jar not found under $SDK/platforms." >&2
    exit 1
fi

echo "==> SDK:         $SDK"
echo "==> NDK:         $ANDROID_NDK_HOME"
echo "==> Build-Tools: $BUILD_TOOLS"
echo "==> Platform:    $ANDROID_JAR"

# 2. Ensure pinned third-party dependencies & original APK are present
if [ -f "$ORIG_APK" ]; then
    "$ROOT/scripts/fetch_deps.sh"
else
    "$ROOT/scripts/fetch_deps.sh" --with-apk "$ORIG_APK"
fi

if [ -n "$EXPECTED_INPUT_SHA256" ]; then
    echo "$EXPECTED_INPUT_SHA256  $ORIG_APK" | sha256sum -c --quiet - || {
        echo "ERROR: Input APK SHA-256 does not match the expected $GD_VERSION build" >&2
        exit 1
    }
fi
echo "==> Building Geometry Dash $GD_VERSION from $ORIG_APK"

WORK="$ROOT/.work_apk"
rm -rf "$WORK"
mkdir -p "$WORK" "$(dirname "$OUT_APK")"

# 3. Build ARM32 guest runtime (libguestrt.so, including dlmalloc + stb_sprintf + zlib 1.3.1)
echo "==> Building ARM32 guest runtime (libguestrt.so)"
"$ROOT/guest/build.sh" "$WORK/libguestrt.so"

# 4. Build ARM64 host bridge (lib/arm64-v8a/libgame.so, 16 KB page-aligned)
echo "==> Building ARM64 host bridge (libgame.so)"
cmake -S "$ROOT/host" -B "$WORK/cmake_host" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$WORK/cmake_host" --parallel

LLVM_STRIP="$(ls -1 "$ANDROID_NDK_HOME"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -n 1)"
cp "$WORK/cmake_host/libgame.so" "$WORK/libgame_arm64.so"
"$LLVM_STRIP" --strip-unneeded "$WORK/libgame_arm64.so"

# 5. Compile ModernPlatformHelper.java & ModernMusicController.java -> classes2.dex
echo "==> Compiling ModernPlatformHelper & ModernMusicController -> classes2.dex"
mkdir -p "$WORK/javac_classes" "$WORK/dex"
javac --release 11 -Xlint:-options -classpath "$ANDROID_JAR" \
    -d "$WORK/javac_classes" \
    "$ROOT/java/com/robtopx/geometryjump/"*.java
"$BUILD_TOOLS/d8" --min-api 24 --lib "$ANDROID_JAR" \
    --output "$WORK/dex" \
    "$WORK/javac_classes/com/robtopx/geometryjump/"*.class

# 6. Decode original APK, relocate original ARM32 libgame.so to assets/arm32/, patch manifest & smali
echo "==> Decoding original APK via Apktool"
java -jar "$ROOT/third_party/apktool.jar" d -f "$ORIG_APK" -o "$WORK/decoded"

mkdir -p "$WORK/decoded/assets/arm32" "$WORK/decoded/lib/arm64-v8a"
mv "$WORK/decoded/lib/armeabi/libgame.so" "$WORK/decoded/assets/arm32/libgame.so"
rm -rf "$WORK/decoded/lib/armeabi" "$WORK/decoded/lib/armeabi-v7a"
cp "$WORK/libguestrt.so" "$WORK/decoded/assets/arm32/libguestrt.so"
cp "$WORK/libgame_arm64.so" "$WORK/decoded/lib/arm64-v8a/libgame.so"

if [ "$LOCAL_GDPS" = "1" ]; then
    mkdir -p "$WORK/decoded/assets/gdps"
    cp "$GDPS_DB" "$WORK/decoded/assets/gdps/levels_1_0.bin"
    echo "==> Embedded private GD 1.0 GDPS level archive: $(stat -c%s "$GDPS_DB") bytes ($(sha256sum "$GDPS_DB" | cut -d' ' -f1))"
fi

echo "==> Applying Android 16 + ARM64 manifest & smali patches (package=$PACKAGE_NAME, app_name='$APP_NAME', local_gdps=$LOCAL_GDPS, version=$GD_VERSION)"
python3 "$ROOT/scripts/patch_manifest_and_smali.py" "$WORK/decoded" \
    --package "$PACKAGE_NAME" \
    --app-name "$APP_NAME" \
    --gd-version "$GD_VERSION" \
    --local-gdps "$LOCAL_GDPS"

echo "==> Rebuilding APK via Apktool"
java -jar "$ROOT/third_party/apktool.jar" b "$WORK/decoded" -o "$WORK/rebuilt_raw.apk"

# 7. Inject classes2.dex and enforce ZIP_STORED on .so, resources.arsc, .mp3, and .ogg
echo "==> Injecting classes2.dex and normalizing STORED entries"
python3 - "$WORK/rebuilt_raw.apk" "$WORK/dex/classes.dex" "$WORK/repacked.apk" << 'PYEOF'
import sys, zipfile

src_apk, classes2_dex, dst_apk = sys.argv[1], sys.argv[2], sys.argv[3]

def should_store(name: str) -> bool:
    if name == "resources.arsc":
        return True
    if name.startswith("lib/") and name.endswith(".so"):
        return True
    if name.startswith("assets/arm32/") and name.endswith(".so"):
        return True
    if name.startswith("assets/gdps/") and name.endswith(".bin"):
        return True
    lower = name.lower()
    return lower.endswith((".mp3", ".ogg", ".png"))

with zipfile.ZipFile(src_apk, "r") as zin, zipfile.ZipFile(dst_apk, "w") as zout:
    for info in zin.infolist():
        if info.filename.startswith("META-INF/"):
            continue
        data = zin.read(info.filename)
        ni = zipfile.ZipInfo(info.filename, date_time=(2026, 1, 1, 0, 0, 0))
        ni.external_attr = 0o644 << 16
        ni.compress_type = zipfile.ZIP_STORED if should_store(info.filename) else zipfile.ZIP_DEFLATED
        zout.writestr(ni, data, compress_type=ni.compress_type)

    with open(classes2_dex, "rb") as f:
        dex_bytes = f.read()
    di = zipfile.ZipInfo("classes2.dex", date_time=(2026, 1, 1, 0, 0, 0))
    di.external_attr = 0o644 << 16
    di.compress_type = zipfile.ZIP_DEFLATED
    zout.writestr(di, dex_bytes, compress_type=zipfile.ZIP_DEFLATED)
PYEOF

# 8. 16 KiB page-align native libraries & 4-byte align uncompressed assets
echo "==> Running zipalign -P 16 4"
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$WORK/repacked.apk" "$WORK/aligned.apk"

# 9. Sign APK (v1 + v2 + v3 schemes)
KS_PATH="${GD102_KEYSTORE:-$WORK/release.keystore}"
KS_PASS="${GD102_KEYSTORE_PASS:-android}"
KS_ALIAS="${GD102_KEY_ALIAS:-gd102arm64}"
if [ ! -f "$KS_PATH" ]; then
    keytool -genkeypair -noprompt \
        -keystore "$KS_PATH" \
        -storepass "$KS_PASS" \
        -keypass "$KS_PASS" \
        -alias "$KS_ALIAS" \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=GD102ARM64, OU=AndroidPort, O=OpenSource, L=Tempe, S=AZ, C=US"
fi

echo "==> Signing APK (v1 + v2 + v3)"
"$BUILD_TOOLS/apksigner" sign \
    --ks "$KS_PATH" \
    --ks-key-alias "$KS_ALIAS" \
    --ks-pass "pass:$KS_PASS" \
    --key-pass "pass:$KS_PASS" \
    --v1-signing-enabled true \
    --v2-signing-enabled true \
    --v3-signing-enabled true \
    --out "$OUT_APK" \
    "$WORK/aligned.apk"

# 10. Verify alignment, signatures, package/label badging, ELF64 16 KB segments, exports, and 136 game assets
echo "==> Verifying signed APK"
"$BUILD_TOOLS/zipalign" -c -P 16 4 "$OUT_APK"
"$BUILD_TOOLS/apksigner" verify --verbose "$OUT_APK"
BADGING="$("$BUILD_TOOLS/aapt2" dump badging "$OUT_APK")"
echo "$BADGING" | grep -E "^(package:|application-label:|launchable-activity:|native-code:)"
echo "$BADGING" | grep -Fq "package: name='${PACKAGE_NAME}'" || {
    echo "ERROR: APK package name does not match '${PACKAGE_NAME}'" >&2
    exit 1
}
echo "$BADGING" | grep -Fq "application-label:'${APP_NAME}'" || {
    echo "ERROR: APK application-label does not match '${APP_NAME}'" >&2
    exit 1
}
echo "$BADGING" | grep -Fq "launchable-activity: name='com.robtopx.geometryjump.GeometryJump'" || {
    echo "ERROR: APK launchable-activity missing com.robtopx.geometryjump.GeometryJump" >&2
    exit 1
}
python3 "$ROOT/scripts/verify_apk.py" "$OUT_APK" --local-gdps "$LOCAL_GDPS" --orig-apk "$ORIG_APK"

rm -rf "$WORK"
echo "==> Built verified ARM64 APK: $OUT_APK"
