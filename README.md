# Geometry Dash 1.02 — Native ARM64 Port for Modern Android (Android 16 / 16 KB Pages)

A complete, 100%-accurate native **ARM64 (`arm64-v8a`)** port of **Geometry Dash 1.02 (build 4)** packaged as **`com.robtopx.geometryj102`** (**`gd 1.02 arm64`**) so it installs side-by-side alongside regular Geometry Dash (`com.robtopx.geometryjump`) with isolated save data — built for modern 64-bit-only Android devices including **Android 16 (API 36)**, **ARMv9 CPUs with hardware AArch32 execution removed** (Google Pixel 7 / 8 / 9 / 10, Snapdragon 8 Gen 3 / 8 Elite, Tensor G3–G5), and **16 KB kernel page sizes**.

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
   - `BadPaddingException` / complete save-data loss on every restart in `SimpleCrypto.getRawKey()`, which relied on Android 4.x Harmony `SecureRandom.getInstance("SHA1PRNG").setSeed(...)` deterministic seeding (on Android 7–16 Conscrypt, `setSeed` mixes `/dev/urandom` entropy and derives a random AES key each launch).
   - `IllegalArgumentException` in `Cocos2dxBitmap.createTextBitmap()` when creating `0x0` bitmaps for empty `CCLabelTTF` strings.
   - Level music restarting from `0:00` on unpause in `Cocos2dxMusic` (`org.cocos2dx.lib.p`), whose 2013 `stop() + prepare() + seekTo(0) + start()` -> `stop() + prepare() + seekTo(ms) + start()` sequence triggers an asynchronous `seekToAsync(0)` race in modern Android `NuPlayerDriver`.
   - Android 16 predictive back gesture intercepting level-pause / editor back navigation (`enableOnBackInvokedCallback="false"`).
   - Cleartext HTTP blocked by default on API 28+ (`usesCleartextTraffic="true"`).

---

## Architecture

Instead of approximating game physics in a partial rewrite, this port executes the **100% unmodified original Geometry Dash 1.02 `libgame.so`** at native speed inside a purpose-built ARM64 host engine (`lib/arm64-v8a/libgame.so`):

| Component | Path | Description |
|---|---|---|
| **ARM64 JIT & Re-Entrant CPU Core** | `host/src/cpu.{h,cpp}` | Patched [Dynarmic](https://github.com/azahar-emu/dynarmic) A32->AArch64 JIT (`ArchVersion::v8`, ARM64 `FastDispatch` inline assembly, CP15 TLS `TPIDRURO`/`TPIDRURW`, full register/VFP save & restore across nested JNI->Java->JNI callbacks). |
| **16 KB Page-Safe 4 GiB Address Space** | `host/src/mem.cpp` | Continuous 4 GiB zero-masked guest arena (`g_base + u32`) with sub-host-page zeroing in `mem::unmap` so 4 KB guest `dlmalloc` `MMAP_CLEARS=1` invariants hold on both 4 KB and 16 KB Android 16 kernels. |
| **ELF32 Relocator & Unwind Resolver** | `host/src/loader.{h,cpp}` | Loads `assets/arm32/libguestrt.so` and `assets/arm32/libgame.so` directly from uncompressed APK memory (`AAsset_getBuffer`), applies ARM32 relocations, exposes `PT_ARM_EXIDX` to `__gnu_Unwind_Find_exidx`, and runs all 226 C++ static constructors. |
| **32<->64 JNI Bridge** | `host/src/jni_bridge.{h,cpp}` | Full 233-entry `JNINativeInterface` + `JavaVM` vtable translator with cached app `ClassLoader` fallback for native `pthread` workers, ART `CheckJNI` pending-exception clearing, local-ref slot reuse, and Modified UTF-8 sanitization. |
| **Native ARM64 GLES2, Bionic & Socket Thunks** | `host/src/thunks_*.cpp` | Zero-overhead ARM64 thunks for all 69 OpenGL ES 2.0 imports (client-array vs VBO pointer tracking + surface-recreation state reset), Bionic LP32 `stat`/`dirent`/`tm`/`timeb`/`addrinfo`/`__fpclassifyd`/`arc4random`/XSI `strerror_r`, `pthread`/semaphores, and full IPv4 + IPv6/NAT64 POSIX sockets & DNS. |
| **ARM32 Guest Runtime + `zlib` 1.3.1** | `guest/` | Self-contained ARMv7 VFPv3-D16 `libguestrt.so` providing `setjmp`/`longjmp`, EABI runtime helpers, Bionic `_ctype_`/`__sF`, `dlmalloc`, `stb_sprintf` stdio, `getopt`, `memrchr`, and full `zlib` 1.3.1 (`inflate*`, `deflate*`, `gz*`, `crc32`, `uncompress`). |
| **Android 16 Platform & Audio Shim** | `java/.../ModernPlatformHelper.java`, `java/.../ModernMusicController.java`, `scripts/patch_manifest_and_smali.py` | Targets API 36 (`minSdkVersion=24`, `targetSdkVersion=36`) with `WindowInsetsController` edge-to-edge sticky immersive fullscreen, `DISPLAY_CUTOUT_MODE_ALWAYS`, `setPreserveEGLContextOnPause(true)`, race-free `ModernMusicController` (`SEEK_CLOSEST` + `USAGE_GAME`, preserving exact paused decoder position on unpause), deterministic AES-128 atomic save persistence (`SimpleCrypto` replacement + multi-user data path virtualization), `ANDROID_ID`-based persistent UUID, `NetworkCapabilities`, `RECEIVER_EXPORTED`, and neutralized legacy Flurry/Chartboost SDKs. |
| **Private Offline 1.0 GDPS (`com.robtopx.geomgdps102`)** | `host/src/local_gdps.{h,cpp}`, `scripts/build_gdhistory_db.py`, `tests/guest_gdps_probe.c` | Embeds every archived 1.0-era online level (IDs `128..1941`) from [GD History](https://history.geometrydash.eu) as a compact `GD10DB01` archive (`assets/gdps/levels_1_0.bin`), redirects `www.boomlings.com` DNS + TCP to an in-process read-only HTTP server on `127.0.0.1` (random loopback port), rewrites post-1.0 level strings back to native `kS1..kA1` format, and hard-blocks every `upload*`/`delete*` endpoint. |

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
dist/gd-1.02-arm64-com.robtopx.geometryj102.apk
```

(You can also override `GD102_PACKAGE_NAME` and `GD102_APP_NAME` before running `./scripts/build_apk.sh` if you want a custom package ID or label.)

To verify any built APK manually:

```bash
python3 scripts/verify_apk.py dist/gd-1.02-arm64-com.robtopx.geometryj102.apk
```

---

## Native ARM64 Headless Runtime Verification

`tests/gd102_verify_runner.cpp` boots a real OpenJDK `JavaVM` + Mesa headless `1280x720` OpenGL ES 2.0 Pbuffer context on Linux ARM64, loads `libguestrt.so` and unmodified `libgame.so` through the Dynarmic ARM64 JIT, and verifies:

- `0` unresolved symbols across all 284 `UND` imports of `libgame.so`
- All `226` C++ static initializers execute cleanly
- 16 KB page-size sub-page `mem::unmap` zeroing invariant
- `LoadingLayer` asset decompression (`zlib` + `libpng` + `.plist` + `.fnt`) -> `MenuLayer` transition (`BaseRobTopActivity.loadingFinished()` + `menuLoop.mp3`)
- `1280x720` OpenGL ES 2.0 framebuffer pixel & color-bucket validation (`GL_NO_ERROR`)
- Re-entrant JNI `Cocos2dxBitmap.createTextBitmap` -> `nativeInitBitmapDC` callbacks
- Touch & hardware Back key (`KEYCODE_BACK`) navigation (`MenuLayer` -> `LevelSelectLayer` -> `PlayLayer`)
- Deterministic AES-128 encrypted save persistence & reload decryption round-trip (`CCGameManager.dat` and `CCGameStore.dat`)

---

## Private Offline Geometry Dash 1.0 Level Server (`com.robtopx.geomgdps102`)

> **Status: disabled for now.** The implementation below is complete and still builds
> locally, but the CI job that assembles the level archive from
> [GD History](https://history.geometrydash.eu) cannot get through Cloudflare from
> hosted runners, so the variant is switched off in the pipeline. Set the repository
> variable `ENABLE_GDPS_VARIANT=true` to re-enable `build-level-archive` and the third
> APK; until then releases contain only the two official-server variants.

The `v1.0.4` work adds a third, fully co-installable APK variant that ships its own
**private, completely offline Geometry Dash 1.0 level server** inside the app:

```text
dist/gd-1.02-gdps-com.robtopx.geomgdps102.apk   (package com.robtopx.geomgdps102, label "gd 1.02 gdps arm64")
```

### What is inside

* **All archived 1.0 levels.** `scripts/build_gdhistory_db.py` walks GD History's API for every
  level ID in `128..1941`, and for each level picks its **earliest archived revision**
  (a pure-1.0 level string whenever one was preserved). Levels with no preserved data are the only
  ones skipped, so the archive is the complete playable 1.0 catalogue (~420 levels).
* **Native 1.02 level format.** Revisions archived from later versions (1.9-2.2) are converted back to
  `kS1..kS6,kA1` + `;id,x,y,...;` objects: 2.0 colour triggers targeting the BG/ground are mapped back to
  the 1.0 `29`/`30` triggers, `kS29`/`kS30`/`kS38` colours are recovered, and objects that do not exist in
  Geometry Dash 1.02's `ObjectToolbox` (verified against the real `GJ_GameSheet.plist` of
  `Geometry.Dash.ver.1.02.build.4.apk`) are dropped.
* **Deterministic binary archive.** `GD10DB01` (`assets/gdps/levels_1_0.bin`) stores one fixed 48-byte
  header + names/description/creator + `zlib`-deflated level string per level; the ARM64 bridge memory-maps
  it straight out of the APK (`AAsset_getBuffer`) and inflates a level string only when it is requested.
* **Loopback HTTP server, no Wi-Fi required.** `gethostbyname("www.boomlings.com")` / `getaddrinfo` are answered
  with the virtual address `127.0.0.77`, and `connect()` transparently rewrites that address to
  `127.0.0.1:<random port>` where the in-process server lives. The unmodified original `libgame.so` therefore
  searches, pages through and downloads real 1.0 levels entirely offline.
* **Read-only by design.** `uploadGJLevel.php`, `uploadGJComment.php`, `uploadGJLevelList.php`,
  `deleteGJLevelUser.php` and every other `upload*`/`delete*` endpoint always answer `-1` and are counted as
  blocked. `likeGJLevel.php`, `rateGJLevel.php` and download counters are accepted but persisted only in the
  app's private `files`/`data` directory (`gdps_local_state.txt`).

### Supported endpoints (GD 1.0 wire format)

`getGJLevels.php` (`type` 0-6, `str`, `diff`, `len`, `page`), `downloadGJLevel.php`, `updateGJLevel.php`,
`likeGJLevel.php` / `likeGJItem.php`, `rateGJLevel.php`, `updateGJUserName.php`, `checkMoreGames.php`.

### Building the archive + APK

```bash
# 1. Fetch every archived 1.0 level (needs internet once; results are cached in /tmp/gdhistory_cache)
python3 scripts/build_gdhistory_db.py \
    --out third_party/levels_1_0.bin \
    --orig-apk third_party/Geometry.Dash.ver.1.02.build.4.apk \
    --cache-dir /tmp/gdhistory_cache

# 2. Build the offline-server variant
GD102_PACKAGE_NAME=com.robtopx.geomgdps102 \
GD102_APP_NAME="gd 1.02 gdps arm64" \
GD102_LOCAL_GDPS=1 \
GD102_GDPS_DB=third_party/levels_1_0.bin \
  ./scripts/build_apk.sh

# 3. Verify (checks package, 16 KB alignment, JNI exports, and the GD10DB01 archive integrity)
python3 scripts/verify_apk.py dist/gd-1.02-gdps-com.robtopx.geomgdps102.apk --local-gdps 1
```

CI additionally proves the feature works end-to-end on real ARM64: it boots the GDPS APK, runs a real
ARM32 guest probe (`tests/guest_gdps_probe.c`) that performs `gethostbyname("www.boomlings.com")` ->
`socket()` -> `connect()` -> HTTP POST for search/download/upload-blocked/like/update, and then drives
`libgame.so`'s own HTTP stack so an actual level download lands in the game's `LevelInfoLayer`.

---

## License & Credits

- **Port & Bridge Code** (`host/`, `java/`, `scripts/`, `tests/`): [MIT License](LICENSE).
- **Third-Party Open-Source Components**:
  - [azahar-emu/dynarmic](https://github.com/azahar-emu/dynarmic) — 0BSD License
  - [27hectormanuel/cq64](https://github.com/27hectormanuel/cq64) guest runtime & fast-dispatch patch — CC0-1.0 Public Domain
  - [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) by Doug Lea & [stb_sprintf.h](https://github.com/nothings/stb) — CC0 / MIT-0 Public Domain
  - [zlib 1.3.1](https://github.com/madler/zlib) by Jean-loup Gailly and Mark Adler — Zlib License
- **Geometry Dash** game assets and original ARM32 binary belong to **RobTop Games AB**.
