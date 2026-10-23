# Geometry Dash 1.02 — Native ARM64 Port for Modern Android (Android 16 / 16 KB Pages)

A complete, 100%-accurate native **ARM64 (`arm64-v8a`)** port of **Geometry Dash 1.02 (build 4)** (`com.robtopx.geometryjump`) built for modern 64-bit-only Android devices — including **Android 16 (API 36)**, **ARMv9 CPUs with hardware AArch32 execution removed** (Google Pixel 7 / 8 / 9 / 10, Snapdragon 8 Gen 3 / 8 Elite, Tensor G3–G5), and **16 KB kernel page sizes**.

[![Download Latest APK Release](https://img.shields.io/github/v/release/rubiidev18alt/gd1dot0arm64?label=Download%20ARM64%20APK&style=for-the-badge)](https://github.com/rubiidev18alt/gd1dot0arm64/releases/latest)

---

## Why Geometry Dash 1.02 Needed an ARM64 Bridge

The original August 2013 release of Geometry Dash 1.02 (`versionCode=4`, `targetSdkVersion=16`) shipped only a single 32-bit `lib/armeabi/libgame.so` binary (`SHA-256: 787b44036ee6f155168b7879f8fab1b733826a0442dd3eec70b157e7e75979db`) with no public source code, plus 2013-era Java/Android APIs that fail to install or crash immediately on modern Android:

1. **No 64-bit native code (`INSTALL_FAILED_NO_MATCHING_ABIS`)**: Modern ARMv9 phones (Pixel 7–10, Galaxy S24/S25/S26, Xiaomi 14/15) drop hardware 32-bit EL0 support entirely.
2. **Android 15/16 16 KB Page Sizes**: Unaligned ELF segments and 4 KB `madvise` assumptions break or corrupt heap memory on 16 KB page-size kernels.
3. **Android 14–16 Target SDK & Runtime Restrictions**:
   - `INSTALL_PARSE_FAILED_MANIFEST_MALFORMED` without explicit `android:exported="true"`.
   - `SecurityException` in `BaseRobTopActivity.getUserID()` reading `TelephonyManager.getDeviceId()` without privileged permissions.
   - `SecurityException` in `BaseRobTopActivity.registerReceiver()` without `Context.RECEIVER_EXPORTED` on Android 14+.
   - `NoClassDefFoundError: org/apache/http/util/ByteArrayBuffer` removed from the Android bootclasspath.
   - `IllegalArgumentException` in `Cocos2dxBitmap.createTextBitmap()` when creating `0x0` bitmaps for empty `CCLabelTTF` strings.
   - Android 16 predictive back gesture intercepting level-pause / editor back navigation (`enableOnBackInvokedCallback="false"`).
   - Cleartext HTTP blocked by default on API 28+ (`usesCleartextTraffic="true"`).

---

## Architecture

Instead of approximating game physics in a partial rewrite, this port executes the **100% unmodified original Geometry Dash 1.02 `libgame.so`** at native speed inside a purpose-built ARM64 host engine (`lib/arm64-v8a/libgame.so`):

| Component | Path | Description |
|---|---|---|
| **ARM64 JIT & Re-Entrant CPU Core** | `host/src/cpu.{h,cpp}` | Patched [Dynarmic](https://github.com/azahar-emu/dynarmic) A32->AArch64 JIT (`ArchVersion::v8`, ARM64 `FastDispatch` inline assembly, CP15 TLS `TPIDRURO`/`TPIDRURW`, full register/VFP save & restore across nested JNI->Java->JNI callbacks). |
| **16 KB Page-Safe 4 GiB Address Space** | `host/src/mem.cpp` | Continuous 4 GiB zero-masked guest arena (`g_base + u32`) with sub-host-page zeroing in `mem::unmap` so 4 KB guest `dlmalloc` `MMAP_CLEARS=1` invariants hold on both 4 KB and 16 KB Android 16 kernels. |
| **ELF32 Relocator & Unwind Resolver** | `host/src/loader.{h,cpp}` | Loads `assets/arm32/libguestrt.so` and `assets/arm32/libgame.so` directly from uncompressed APK memory (`AAsset_getBuffer`), applies ARM32 relocations, exposes `PT_ARM_EXIDX` to `__gnu_Unwind_Find_exidx`, and runs all 227 C++ static constructors. |
| **32<->64 JNI Bridge** | `host/src/jni_bridge.{h,cpp}` | Full 233-entry `JNINativeInterface` + `JavaVM` vtable translator with cached app `ClassLoader` fallback for native `pthread` workers, local-ref slot reuse, and Modified UTF-8 sanitization (4-byte emoji surrogate pairs + Latin-1 high-byte safety for ART `CheckJNI`). |
| **Native ARM64 GLES2, Bionic & Socket Thunks** | `host/src/thunks_*.cpp` | Zero-overhead ARM64 thunks for all 69 OpenGL ES 2.0 imports (client-array vs VBO pointer tracking + surface-recreation state reset), Bionic LP32 `stat`/`dirent`/`tm`/`timeb`/`addrinfo`/`__fpclassifyd`/`arc4random`/XSI `strerror_r`, `pthread`/semaphores, and full IPv4 + IPv6/NAT64 POSIX sockets & DNS. |
| **ARM32 Guest Runtime + `zlib` 1.3.1** | `guest/` | Self-contained ARMv7 VFPv3-D16 `libguestrt.so` providing `setjmp`/`longjmp`, EABI runtime helpers, Bionic `_ctype_`/`__sF`, `dlmalloc`, `stb_sprintf` stdio, `getopt`, `memrchr`, and full `zlib` 1.3.1 (`inflate*`, `deflate*`, `gz*`, `crc32`, `uncompress`). |
| **Android 16 Platform Shim** | `java/.../ModernPlatformHelper.java`, `scripts/patch_manifest_and_smali.py` | Targets API 36 (`minSdkVersion=24`, `targetSdkVersion=36`) with `WindowInsetsController` edge-to-edge sticky immersive fullscreen, `DISPLAY_CUTOUT_MODE_ALWAYS`, `setPreserveEGLContextOnPause(true)`, `ANDROID_ID`-based persistent UUID, `NetworkCapabilities`, `RECEIVER_EXPORTED`, and neutralized legacy Flurry/Chartboost SDKs. |

---

## Building from Source

### Prerequisites

- **Linux (x86_64 or ARM64)** or **macOS**
- **Android SDK** (`platforms;android-36`, `build-tools;36.0.0`) & **Android NDK r26+** (`ANDROID_SDK_ROOT` and `ANDROID_NDK_HOME`)
- `cmake` (>= 3.22), `ninja`, `python3`, **JDK 17+** (`javac`, `keytool`), `git`, `curl`

### One-Command APK Build

```bash
export ANDROID_SDK_ROOT=/path/to/android-sdk
export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/27.2.12479018

# Automatically downloads pinned dependencies (Dynarmic, zlib 1.3.1, Boost headers, Apktool)
# and the SHA-256-verified Geometry.Dash.ver.1.02.build.4.apk if not supplied:
./scripts/build_apk.sh
```

The signed, 16 KB-aligned (`zipalign -P 16 4`), v1+v2+v3 verified APK is written to:

```text
dist/GeometryDash-1.02-arm64-android16.apk
```

To verify any built APK manually:

```bash
python3 scripts/verify_apk.py dist/GeometryDash-1.02-arm64-android16.apk
```

---

## Native ARM64 Headless Runtime Verification

`tests/gd102_verify_runner.cpp` boots a real OpenJDK `JavaVM` + Mesa headless `1280x720` OpenGL ES 2.0 Pbuffer context on Linux ARM64, loads `libguestrt.so` and unmodified `libgame.so` through the Dynarmic ARM64 JIT, and verifies:

- `0` unresolved symbols across all 284 `UND` imports of `libgame.so`
- All `227` C++ static initializers execute cleanly
- 16 KB page-size sub-page `mem::unmap` zeroing invariant
- `LoadingLayer` asset decompression (`zlib` + `libpng` + `.plist` + `.fnt`) -> `MenuLayer` transition (`BaseRobTopActivity.loadingFinished()` + `menuLoop.mp3`)
- `1280x720` OpenGL ES 2.0 framebuffer pixel & color-bucket validation (`GL_NO_ERROR`)
- Re-entrant JNI `Cocos2dxBitmap.createTextBitmap` -> `nativeInitBitmapDC` callbacks
- Touch input navigation (`MenuLayer` -> `LevelSelectLayer` -> `PlayLayer` Stereo Madness)
- Encrypted save persistence (`CCGameManager.dat` and `CCLocalLevels.dat`)

---

## License & Credits

- **Port & Bridge Code** (`host/`, `java/`, `scripts/`, `tests/`): [MIT License](LICENSE).
- **Third-Party Open-Source Components**:
  - [azahar-emu/dynarmic](https://github.com/azahar-emu/dynarmic) — 0BSD License
  - [27hectormanuel/cq64](https://github.com/27hectormanuel/cq64) guest runtime & fast-dispatch patch — CC0-1.0 Public Domain
  - [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) by Doug Lea & [stb_sprintf.h](https://github.com/nothings/stb) — CC0 / MIT-0 Public Domain
  - [zlib 1.3.1](https://github.com/madler/zlib) by Jean-loup Gailly and Mark Adler — Zlib License
- **Geometry Dash** game assets and original ARM32 binary belong to **RobTop Games AB**.
