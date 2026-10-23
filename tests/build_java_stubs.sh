#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Compiles minimal standard-JVM JNI target classes into headless_stubs.jar so
# `gd102_verify_runner` can run Geometry Dash 1.02 against a real OpenJDK JavaVM
# and Mesa headless OpenGL ES 2.0 on Linux ARM64 without needing Android framework jars.
set -euo pipefail

OUT_JAR="${1:?usage: build_java_stubs.sh <out.jar>}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/src/org/cocos2dx/lib" "$TMP/src/com/customRobTop" "$TMP/classes"

cat > "$TMP/src/org/cocos2dx/lib/Cocos2dxActivity.java" << 'EOF'
package org.cocos2dx.lib;

import java.util.ArrayList;
import java.util.List;

public class Cocos2dxActivity {
    public static volatile String lastMusicPlayed = "";
    public static volatile int musicPlayCount = 0;
    public static volatile int effectPreloadCount = 0;
    public static volatile int effectPlayCount = 0;
    public static float musicVolume = 1.0f;
    public static float effectsVolume = 1.0f;
    public static boolean musicPlaying = false;
    public static final List<String> events = new ArrayList<>();

    public static native void nativeSetPaths(String apkPath);

    public static String getCocos2dxPackageName() { return "com.robtopx.geometryj102"; }
    public static String getCurrentLanguage() { return "en"; }

    public static synchronized void preloadBackgroundMusic(String path) {
        events.add("preloadMusic:" + path);
    }
    public static synchronized void playBackgroundMusic(String path, boolean loop) {
        lastMusicPlayed = path != null ? path : "";
        musicPlaying = true;
        musicPlayCount++;
        events.add("playMusic:" + path);
    }
    public static void stopBackgroundMusic() { musicPlaying = false; }
    public static void pauseBackgroundMusic() { musicPlaying = false; }
    public static void resumeBackgroundMusic() { musicPlaying = true; }
    public static void rewindBackgroundMusic() {}
    public static boolean isBackgroundMusicPlaying() { return musicPlaying; }
    public static float getBackgroundMusicVolume() { return musicVolume; }
    public static void setBackgroundMusicVolume(float v) { musicVolume = v; }
    public static void setBackgroundMusicTime(float t) {}

    public static synchronized void preloadEffect(String path) {
        effectPreloadCount++;
    }
    public static synchronized int playEffect(String path, boolean loop, float pitch, float pan, float gain) {
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
    public static void end() {}
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
    "$TMP/src/org/cocos2dx/lib/"*.java \
    "$TMP/src/com/customRobTop/"*.java

mkdir -p "$(dirname "$OUT_JAR")"
jar --create --file "$OUT_JAR" -C "$TMP/classes" .
echo "==> Built $OUT_JAR"
