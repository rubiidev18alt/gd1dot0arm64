#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compiles minimal standard-JVM JNI target classes + the REAL production
# `ModernMusicController.java` into headless_stubs.jar so `gd102_verify_runner`
# exercises Geometry Dash 1.02's exact pause/unpause audio state machine against
# a real OpenJDK JavaVM and Mesa headless OpenGL ES 2.0 on Linux ARM64.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_JAR="${1:?usage: build_java_stubs.sh <out.jar>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p \
    "$TMP/src/android/content/res" \
    "$TMP/src/android/media" \
    "$TMP/src/android/os" \
    "$TMP/src/android/util" \
    "$TMP/src/org/cocos2dx/lib" \
    "$TMP/src/com/customRobTop" \
    "$TMP/classes"

cat > "$TMP/src/android/content/Context.java" << 'EOF'
package android.content;

import android.content.res.AssetManager;

public class Context {
    public static final Context INSTANCE = new Context();
    private final AssetManager assets = new AssetManager();

    public Context getApplicationContext() { return this; }
    public AssetManager getAssets() { return assets; }
}
EOF

cat > "$TMP/src/android/content/res/AssetFileDescriptor.java" << 'EOF'
package android.content.res;

import java.io.Closeable;
import java.io.FileDescriptor;

public class AssetFileDescriptor implements Closeable {
    public static volatile int openCount = 0;
    public static volatile int closeCount = 0;
    private final String assetPath;
    private boolean closed = false;

    public AssetFileDescriptor(String assetPath) {
        this.assetPath = assetPath;
        openCount++;
    }

    public FileDescriptor getFileDescriptor() { return new FileDescriptor(); }
    public long getStartOffset() { return 0L; }
    public long getLength() { return 4_000_000L; }
    public String getAssetPath() { return assetPath; }

    @Override
    public void close() {
        if (!closed) {
            closed = true;
            closeCount++;
        }
    }
}
EOF

cat > "$TMP/src/android/content/res/AssetManager.java" << 'EOF'
package android.content.res;

public class AssetManager {
    public AssetFileDescriptor openFd(String fileName) {
        return new AssetFileDescriptor(fileName);
    }
}
EOF

cat > "$TMP/src/android/media/AudioAttributes.java" << 'EOF'
package android.media;

public final class AudioAttributes {
    public static final int USAGE_MEDIA = 1;
    public static final int USAGE_GAME = 14;
    public static final int CONTENT_TYPE_MUSIC = 2;

    public final int usage;
    public final int contentType;

    private AudioAttributes(int usage, int contentType) {
        this.usage = usage;
        this.contentType = contentType;
    }

    public static final class Builder {
        private int usage = USAGE_MEDIA;
        private int contentType = CONTENT_TYPE_MUSIC;

        public Builder setUsage(int u) { this.usage = u; return this; }
        public Builder setContentType(int c) { this.contentType = c; return this; }
        public AudioAttributes build() { return new AudioAttributes(usage, contentType); }
    }
}
EOF

cat > "$TMP/src/android/media/MediaPlayer.java" << 'EOF'
package android.media;

import java.io.FileDescriptor;

/**
 * Stateful MediaPlayer test double modeling Android 10-16 NuPlayerDriver:
 * - Tracks simulated playback position via {@link #advanceClockMs(int)}.
 * - Reproduces NuPlayerDriver's async `seekTo(0)` + `stop()/prepare()` race:
 *   if `stop()` + `prepare()` is called while a `seekTo(0)` is pending from
 *   the same turn, position snaps back to 0 and increments {@link #nuPlayerRaceResets}.
 */
public class MediaPlayer {
    public static final int SEEK_PREVIOUS_SYNC = 0;
    public static final int SEEK_CLOSEST = 3;

    public static volatile MediaPlayer latestInstance = null;
    public static volatile int stopCalls = 0;
    public static volatile int seekZeroCalls = 0;
    public static volatile int seekNonZeroCalls = 0;
    public static volatile int lastSeekTargetMs = -1;
    public static volatile int lastSeekMode = -1;
    public static volatile int nuPlayerRaceResets = 0;

    private boolean prepared = false;
    private boolean playing = false;
    private boolean stopped = false;
    private boolean released = false;
    private boolean looping = false;
    private boolean pendingAsyncZeroSeek = false;
    private int positionMs = 0;
    private int durationMs = 120_000;
    private float leftVol = 1.0f;
    private float rightVol = 1.0f;

    public MediaPlayer() {
        latestInstance = this;
    }

    public static synchronized void resetTelemetry() {
        stopCalls = 0;
        seekZeroCalls = 0;
        seekNonZeroCalls = 0;
        lastSeekTargetMs = -1;
        lastSeekMode = -1;
        nuPlayerRaceResets = 0;
    }

    public static synchronized void advanceClockMs(int deltaMs) {
        MediaPlayer mp = latestInstance;
        if (mp != null && !mp.released && mp.playing && deltaMs > 0) {
            mp.positionMs += deltaMs;
            if (mp.positionMs >= mp.durationMs) {
                mp.positionMs = mp.looping ? (mp.positionMs % mp.durationMs) : mp.durationMs;
            }
            mp.pendingAsyncZeroSeek = false;
        }
    }

    public static synchronized int activePositionMs() {
        MediaPlayer mp = latestInstance;
        return (mp != null && !mp.released) ? mp.positionMs : -1;
    }

    public void setAudioAttributes(AudioAttributes attrs) {}
    public void setDataSource(String path) {}
    public void setDataSource(FileDescriptor fd, long offset, long length) {}

    public synchronized void prepare() {
        if (released) throw new IllegalStateException("released");
        if (stopped && pendingAsyncZeroSeek) {
            // Exact Android NuPlayerDriver::prepare_l(STATE_STOPPED) + pending kWhatSeek(0) race!
            nuPlayerRaceResets++;
            positionMs = 0;
        } else if (stopped) {
            positionMs = 0;
        }
        prepared = true;
        stopped = false;
    }

    public synchronized void start() {
        if (released || !prepared || stopped) throw new IllegalStateException("not prepared");
        playing = true;
    }

    public synchronized void pause() {
        if (released) throw new IllegalStateException("released");
        playing = false;
    }

    public synchronized void stop() {
        stopCalls++;
        playing = false;
        stopped = true;
    }

    public synchronized void release() {
        playing = false;
        prepared = false;
        released = true;
    }

    public synchronized void seekTo(int msec) {
        seekTo((long) msec, SEEK_PREVIOUS_SYNC);
    }

    public synchronized void seekTo(long msec, int mode) {
        if (released || !prepared || stopped) throw new IllegalStateException("bad state for seekTo");
        int target = (int) Math.max(0L, msec);
        lastSeekTargetMs = target;
        lastSeekMode = mode;
        if (target == 0) {
            seekZeroCalls++;
            pendingAsyncZeroSeek = true;
            positionMs = 0;
        } else {
            seekNonZeroCalls++;
            if (nuPlayerRaceResets > 0) {
                positionMs = 0;
            } else {
                positionMs = target;
            }
        }
    }

    public synchronized int getCurrentPosition() { return positionMs; }
    public synchronized int getDuration() { return durationMs; }
    public synchronized boolean isPlaying() { return !released && playing; }
    public synchronized void setLooping(boolean loop) { this.looping = loop; }
    public synchronized void setVolume(float left, float right) {
        this.leftVol = left;
        this.rightVol = right;
    }
}
EOF

cat > "$TMP/src/android/os/Build.java" << 'EOF'
package android.os;

public final class Build {
    public static final class VERSION {
        public static final int SDK_INT = 36;
    }
    public static final class VERSION_CODES {
        public static final int LOLLIPOP = 21;
        public static final int O = 26;
    }
}
EOF

cat > "$TMP/src/android/os/Looper.java" << 'EOF'
package android.os;

public final class Looper {
    public static Looper getMainLooper() { return null; }
}
EOF

cat > "$TMP/src/android/os/Handler.java" << 'EOF'
package android.os;

public class Handler {
    public Handler(Looper looper) {}
    public boolean postDelayed(Runnable r, long delayMillis) { return false; }
}
EOF

cat > "$TMP/src/android/os/SystemClock.java" << 'EOF'
package android.os;

public final class SystemClock {
    private static volatile long extraMs = 0L;
    public static void advanceUptimeMs(long deltaMs) { extraMs += deltaMs; }
    public static long uptimeMillis() {
        return (System.nanoTime() / 1_000_000L) + extraMs;
    }
}
EOF

cat > "$TMP/src/android/util/Log.java" << 'EOF'
package android.util;

public final class Log {
    public static int d(String tag, String msg) { return 0; }
    public static int i(String tag, String msg) { return 0; }
    public static int w(String tag, String msg) { return 0; }
    public static int w(String tag, String msg, Throwable tr) { return 0; }
    public static int e(String tag, String msg) { return 0; }
    public static int e(String tag, String msg, Throwable tr) { return 0; }
}
EOF

cat > "$TMP/src/org/cocos2dx/lib/Cocos2dxActivity.java" << 'EOF'
package org.cocos2dx.lib;

import android.content.Context;
import com.robtopx.geometryjump.ModernMusicController;
import java.util.ArrayList;
import java.util.List;

public class Cocos2dxActivity {
    public static volatile String lastMusicPlayed = "";
    public static volatile int musicPlayCount = 0;
    public static volatile int effectPreloadCount = 0;
    public static volatile int effectPlayCount = 0;
    public static volatile String lastEffectPlayed = "";
    public static volatile float lastEffectPitch = -1.0f;
    public static volatile float lastEffectPan = -999.0f;
    public static volatile float lastEffectGain = -1.0f;
    public static float effectsVolume = 1.0f;
    public static final List<String> events = new ArrayList<>();

    static {
        ModernMusicController.init(Context.INSTANCE);
    }

    public static native void nativeSetPaths(String apkPath);

    public static String getCocos2dxPackageName() { return "com.robtopx.geometryj102"; }
    public static String getCurrentLanguage() { return "en"; }

    public static synchronized void preloadBackgroundMusic(String path) {
        events.add("preload(" + path + ")");
        ModernMusicController.preload(Context.INSTANCE, path);
    }
    public static synchronized void playBackgroundMusic(String path, boolean loop) {
        lastMusicPlayed = path != null ? path : "";
        musicPlayCount++;
        events.add("play(" + path + ",loop=" + loop + ")");
        ModernMusicController.play(Context.INSTANCE, path, loop);
    }
    public static synchronized void stopBackgroundMusic() {
        events.add("stop()");
        ModernMusicController.stop();
    }
    public static synchronized void pauseBackgroundMusic() {
        events.add("pause()");
        ModernMusicController.pause();
    }
    public static synchronized void resumeBackgroundMusic() {
        events.add("resume()");
        ModernMusicController.resume();
    }
    public static synchronized void rewindBackgroundMusic() {
        events.add("rewind()");
        ModernMusicController.rewind();
    }
    public static synchronized boolean isBackgroundMusicPlaying() {
        boolean playing = ModernMusicController.isPlaying();
        events.add("isPlaying()->" + playing);
        return playing;
    }
    public static synchronized float getBackgroundMusicVolume() {
        return ModernMusicController.getVolume();
    }
    public static synchronized void setBackgroundMusicVolume(float v) {
        events.add(String.format("setVol(%.3f)", v));
        ModernMusicController.setVolume(v);
    }
    public static synchronized void setBackgroundMusicTime(float t) {
        events.add(String.format("setTime(%.3f)", t));
        ModernMusicController.setTime(t);
    }
    public static synchronized float getBackgroundMusicTime() {
        float t = ModernMusicController.getTime();
        events.add(String.format("getTime()->%.3f", t));
        return t;
    }
    public static synchronized String drainEvents() {
        String s = String.join(" -> ", events);
        events.clear();
        return s;
    }

    public static synchronized void preloadEffect(String path) {
        effectPreloadCount++;
    }
    public static synchronized int playEffect(String path, boolean loop, float pitch, float pan, float gain) {
        lastEffectPlayed = path != null ? path : "";
        lastEffectPitch = pitch;
        lastEffectPan = pan;
        lastEffectGain = gain;
        effectPlayCount++;
        return effectPlayCount;
    }
    public static void stopEffect(int id) {}
    public static void pauseEffect(int id) {}
    public static void resumeEffect(int id) {}
    public static void unloadEffect(String path) {}
    public static float getEffectsVolume() { return effectsVolume; }
    public static void setEffectsVolume(float v) { effectsVolume = v; }
    public static void pauseAllEffects() {}
    public static void resumeAllEffects() {}
    public static void stopAllEffects() {}

    public static void enableAccelerometer() {}
    public static void disableAccelerometer() {}
    public static void showMessageBox(String title, String message) {}
    public static void end() { ModernMusicController.end(); }
    public static void terminateProcess() {}
}
EOF

cat > "$TMP/src/org/cocos2dx/lib/Cocos2dxBitmap.java" << 'EOF'
package org.cocos2dx.lib;

public class Cocos2dxBitmap {
    public static volatile int bitmapCalls = 0;

    private static native void nativeInitBitmapDC(int width, int height, byte[] pixels);

    public static void createTextBitmap(String text, String fontName, int fontSize,
                                        int alignment, int width, int height) {
        bitmapCalls++;
        if (text == null) text = "";
        int w = width > 0 ? width : Math.max(4, text.length() * Math.max(4, fontSize / 2));
        int h = height > 0 ? height : Math.max(4, fontSize + 4);
        w = Math.min(Math.max(1, w), 1024);
        h = Math.min(Math.max(1, h), 512);
        byte[] pixels = new byte[w * h * 4];
        if (!text.isEmpty()) {
            for (int i = 0; i < pixels.length; i += 4) {
                pixels[i]     = (byte) 0xFF;
                pixels[i + 1] = (byte) 0xFF;
                pixels[i + 2] = (byte) 0xFF;
                pixels[i + 3] = (byte) 0xFF;
            }
        }
        // Re-enters guest ARM32 execution inside an active JNI CallStaticVoidMethod SVC!
        nativeInitBitmapDC(w, h, pixels);
    }
}
EOF

cat > "$TMP/src/org/cocos2dx/lib/Cocos2dxGLSurfaceView.java" << 'EOF'
package org.cocos2dx.lib;

public class Cocos2dxGLSurfaceView {
    public static void openIMEKeyboard() {}
    public static void closeIMEKeyboard() {}
}
EOF

cat > "$TMP/src/org/cocos2dx/lib/Cocos2dxRenderer.java" << 'EOF'
package org.cocos2dx.lib;

public class Cocos2dxRenderer {
    public static volatile double animationInterval = 1.0 / 60.0;
    public static void setAnimationInterval(double interval) { animationInterval = interval; }

    public static native void nativeInit(int w, int h);
    public static native void nativeRender();
    public static native void nativeOnPause();
    public static native void nativeOnResume();
    public static native void nativeTouchesBegin(int id, float x, float y);
    public static native void nativeTouchesEnd(int id, float x, float y);
    public static native void nativeTouchesMove(int[] ids, float[] xs, float[] ys);
    public static native void nativeTouchesCancel(int[] ids, float[] xs, float[] ys);
    public static native boolean nativeKeyDown(int keyCode);
    public static native void nativeInsertText(String text);
    public static native void nativeDeleteBackward();
    public static native String nativeGetContentText();
}
EOF

cat > "$TMP/src/com/customRobTop/DefaultRobTopActivity.java" << 'EOF'
package com.customRobTop;

import org.cocos2dx.lib.Cocos2dxActivity;

public class DefaultRobTopActivity extends Cocos2dxActivity {
    public static boolean isBillingSupported() { return false; }
    public static void purchaseItem(String sku) {}
    public static void restorePurchases() {}
}
EOF

cat > "$TMP/src/com/customRobTop/BillingRobTopActivity.java" << 'EOF'
package com.customRobTop;

public class BillingRobTopActivity extends DefaultRobTopActivity {
    public static boolean isBillingSupported() { return false; }
    public static void purchaseItem(String sku) {}
    public static void restorePurchases() {}
}
EOF

cat > "$TMP/src/com/customRobTop/BaseRobTopActivity.java" << 'EOF'
package com.customRobTop;

public class BaseRobTopActivity extends DefaultRobTopActivity {
    public static volatile boolean loadingFinishedCalled = false;

    public static String admobKey() { return ""; }
    public static String cbAppID() { return ""; }
    public static String cbAppSignature() { return ""; }
    public static void cacheInterstitial() {}
    public static void cacheInterstitial(String loc) {}
    public static boolean hasCachedInterstitial() { return false; }
    public static boolean hasCachedInterstitial(String loc) { return false; }
    public static void showInterstitial() {}
    public static void showInterstitial(String loc) {}
    public static void enableBanner() {}
    public static void disableBanner() {}
    public static void pauseAds() {}
    public static void resumeAds() {}
    public static boolean doesFileExist(String path) { return false; }
    public static void downloadImage(String path, String name, String url) {}
    public static String getUserID() { return "11111111-2222-3333-4444-555555555555"; }
    public static boolean isNetworkAvailable() { return false; }
    public static void loadingFinished() { loadingFinishedCalled = true; }
    public static void logEvent(String ev) {}
    public static void onNativePause() {}
    public static void onNativeResume() {}
    public static void onToggleKeyboard() {}
    public static void setKeyboardState(boolean active) {}
    public static void setBlockBackButton(boolean block) {}
    public static void openAppPage() {}
    public static void openURL(String url) {}
    public static void sendMail(String subject, String body, String to) {}
    public static boolean shouldResumeSound() { return true; }
    public static void tryShowRateDialog(String appName) {}
}
EOF

cat > "$TMP/src/com/customRobTop/JniToCpp.java" << 'EOF'
package com.customRobTop;

public class JniToCpp {
    public static native void didCacheInterstitial(String loc);
    public static native void didClickInterstitial();
    public static native void didCloseInterstitial();
    public static native void didDismissInterstitial();
    public static native void hideLoadingCircle();
    public static native void itemPurchaseFailed(String sku);
    public static native void itemPurchased(String sku);
    public static native void itemRefunded(String sku);
    public static native void promoImageDownloaded();
    public static native void resumeSound();
    public static native void showInterstitialFailed();
}
EOF

cat > "$TMP/src/com/customRobTop/SimpleCrypto.java" << 'EOF'
package com.customRobTop;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

public class SimpleCrypto {
    private static final String[] PREFIXES = {
        "/data/data/com.robtopx.geometryjump",
        "/data/data/com.robtopx.geometryj102"
    };
    private static final String SEED = "y27vyZlpIJk2C8wd";

    private static String resolvePath(String rawPath) {
        if (rawPath == null) return "";
        String dataDir = System.getProperty("gd102.data.dir", "");
        if (!dataDir.isEmpty()) {
            for (String pfx : PREFIXES) {
                if (rawPath.startsWith(pfx)) {
                    return dataDir + rawPath.substring(pfx.length());
                }
            }
        }
        return rawPath;
    }

    private static SecretKeySpec key() throws Exception {
        byte[] d = MessageDigest.getInstance("SHA-256").digest(SEED.getBytes(StandardCharsets.UTF_8));
        return new SecretKeySpec(Arrays.copyOf(d, 16), "AES");
    }

    public static String loadAndDecryptFileToString(String rawPath) {
        try {
            File f = new File(resolvePath(rawPath));
            if (!f.exists() || !f.isFile()) return "";
            byte[] enc;
            try (FileInputStream in = new FileInputStream(f);
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) != -1) out.write(buf, 0, n);
                enc = out.toByteArray();
            }
            if (enc.length == 0) return "";
            if (enc[0] == (byte) '<') {
                return new String(enc, StandardCharsets.UTF_8);
            }
            Cipher c = Cipher.getInstance("AES/ECB/PKCS5Padding");
            c.init(Cipher.DECRYPT_MODE, key());
            return new String(c.doFinal(enc), StandardCharsets.UTF_8);
        } catch (Throwable t) {
            return "";
        }
    }

    public static void saveAndEncryptStringToFile(String plaintext, String rawPath) {
        if (plaintext == null || rawPath == null || rawPath.isEmpty()) return;
        try {
            Cipher c = Cipher.getInstance("AES/ECB/PKCS5Padding");
            c.init(Cipher.ENCRYPT_MODE, key());
            byte[] enc = c.doFinal(plaintext.getBytes(StandardCharsets.UTF_8));
            File dst = new File(resolvePath(rawPath));
            File p = dst.getParentFile();
            if (p != null && !p.exists()) p.mkdirs();
            try (FileOutputStream out = new FileOutputStream(dst)) {
                out.write(enc);
                out.getFD().sync();
            }
        } catch (Throwable ignored) {}
    }
}
EOF

javac --release 11 -d "$TMP/classes" \
    "$TMP/src/android/content/"*.java \
    "$TMP/src/android/content/res/"*.java \
    "$TMP/src/android/media/"*.java \
    "$TMP/src/android/os/"*.java \
    "$TMP/src/android/util/"*.java \
    "$ROOT/java/com/robtopx/geometryjump/ModernMusicController.java" \
    "$TMP/src/org/cocos2dx/lib/"*.java \
    "$TMP/src/com/customRobTop/"*.java

mkdir -p "$(dirname "$OUT_JAR")"
jar --create --file "$OUT_JAR" -C "$TMP/classes" .
echo "==> Built $OUT_JAR"
