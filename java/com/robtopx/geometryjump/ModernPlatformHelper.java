// SPDX-License-Identifier: MIT
package com.robtopx.geometryjump;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.res.AssetManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;
import android.util.Log;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.UUID;

import javax.crypto.Cipher;
import javax.crypto.spec.SecretKeySpec;

/**
 * Android 16 (API 36) platform compatibility & ARM64 bridge bootstrapper for Geometry Dash 1.02.
 */
public final class ModernPlatformHelper {
    private static final String TAG = "gd102-platform";
    private static final String PREFS_NAME = "gd102_modern_prefs";
    private static final String KEY_USER_ID = "user_uuid";
    private static final String LEGACY_DATA_PREFIX = "/data/data/com.robtopx.geometryjump";
    private static final String SAVE_SEED = "y27vyZlpIJk2C8wd";

    private static volatile String sDataDir = null;
    private static volatile String sPkgDataPrefix = null;
    private static volatile boolean sLocalGdps = false;
    private static volatile boolean sRenderPriorityRaised = false;

    private ModernPlatformHelper() {}

    public static native void nativeBridgeInit(
            Context context, AssetManager assetManager, String apkPath, String dataDir);

    public static native void nativeOnSurfaceCreated();

    /** True when the private offline Geometry Dash 1.0 level server is baked into this APK. */
    public static native boolean nativeLocalGdpsEnabled();

    public static boolean isLocalGdpsEnabled() {
        return sLocalGdps;
    }

    public static void initBridge(Activity activity) {
        if (activity == null) return;
        Context app = activity.getApplicationContext();
        if (app == null) app = activity;

        String apkPath = app.getApplicationInfo().sourceDir;
        String dataDir = app.getApplicationInfo().dataDir;
        if (dataDir == null || dataDir.isEmpty()) {
            dataDir = app.getFilesDir().getParentFile().getAbsolutePath();
        }
        sDataDir = dataDir;
        String pkg = app.getPackageName();
        if (pkg != null && !pkg.isEmpty()) {
            sPkgDataPrefix = "/data/data/" + pkg;
        }
        ModernMusicController.init(app);
        nativeBridgeInit(app, app.getAssets(), apkPath, dataDir);
        try {
            sLocalGdps = nativeLocalGdpsEnabled();
        } catch (Throwable t) {
            sLocalGdps = false;
        }
        if (sLocalGdps) {
            Log.i(TAG, "Private offline GD 1.0 level server enabled (all boomlings.com traffic is local)");
        }
        applyImmersive(activity);
        ModernFpsCounter.install(activity);
    }

    /**
     * Called from {@code Cocos2dxRenderer.onDrawFrame} once per rendered frame, i.e. on the GL thread
     * at display refresh rate. Keep this allocation-free: it only raises the render thread's priority
     * once (so low-end devices and emulators schedule frames ahead of background work) and bumps the
     * FPS counter, whose 500 ms sample window runs on the UI thread.
     */
    public static void onFrameRendered() {
        if (!sRenderPriorityRaised) {
            sRenderPriorityRaised = true;
            try {
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_DISPLAY);
            } catch (Throwable ignored) {
                // Some emulators refuse priority changes; frames still render normally.
            }
        }
        ModernFpsCounter.onFrame();
    }

    public static String resolveSavePath(String path) {
        if (path == null) return "";
        String dir = sDataDir;
        if (dir != null && !dir.isEmpty()) {
            for (String prefix : new String[]{LEGACY_DATA_PREFIX, sPkgDataPrefix}) {
                if (prefix != null && !prefix.isEmpty() && path.startsWith(prefix)) {
                    char next = path.length() > prefix.length()
                            ? path.charAt(prefix.length())
                            : '/';
                    if (next == '/') {
                        return dir + path.substring(prefix.length());
                    }
                }
            }
        }
        return path;
    }

    private static SecretKeySpec deriveDeterministicSaveKey() throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        byte[] digest = sha.digest(SAVE_SEED.getBytes(StandardCharsets.UTF_8));
        return new SecretKeySpec(Arrays.copyOf(digest, 16), "AES");
    }

    /**
     * Replaces GD 1.02's broken `SecureRandom.getInstance("SHA1PRNG").setSeed(...)` key derivation,
     * which on Android 7.0–16 (Conscrypt) mixes `/dev/urandom` entropy and generates a different
     * random key on every launch (wiping player progress upon restart).
     */
    public static String loadAndDecryptSaveFile(String rawPath) {
        try {
            File file = new File(resolveSavePath(rawPath));
            if (!file.exists() || !file.isFile()) return "";
            byte[] cipherBytes;
            try (FileInputStream in = new FileInputStream(file);
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) != -1) {
                    out.write(buf, 0, n);
                }
                cipherBytes = out.toByteArray();
            }
            if (cipherBytes.length == 0) return "";
            if (cipherBytes[0] == (byte) '<') {
                return new String(cipherBytes, StandardCharsets.UTF_8);
            }
            Cipher cipher = Cipher.getInstance("AES/ECB/PKCS5Padding");
            cipher.init(Cipher.DECRYPT_MODE, deriveDeterministicSaveKey());
            byte[] plain = cipher.doFinal(cipherBytes);
            return new String(plain, StandardCharsets.UTF_8);
        } catch (Throwable t) {
            Log.w(TAG, "loadAndDecryptSaveFile failed for " + rawPath, t);
            return "";
        }
    }

    public static void saveAndEncryptSaveFile(String plaintext, String rawPath) {
        if (plaintext == null || rawPath == null || rawPath.isEmpty()) return;
        try {
            Cipher cipher = Cipher.getInstance("AES/ECB/PKCS5Padding");
            cipher.init(Cipher.ENCRYPT_MODE, deriveDeterministicSaveKey());
            byte[] encrypted = cipher.doFinal(plaintext.getBytes(StandardCharsets.UTF_8));

            File dst = new File(resolveSavePath(rawPath));
            File parent = dst.getParentFile();
            if (parent != null && !parent.exists()) {
                parent.mkdirs();
            }
            File tmp = new File(dst.getAbsolutePath() + ".tmp");
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(encrypted);
                out.getFD().sync();
            }
            if (!tmp.renameTo(dst)) {
                try (FileOutputStream out = new FileOutputStream(dst)) {
                    out.write(encrypted);
                    out.getFD().sync();
                }
                tmp.delete();
            }
        } catch (Throwable t) {
            Log.e(TAG, "saveAndEncryptSaveFile failed for " + rawPath, t);
        }
    }

    @SuppressWarnings("deprecation")
    public static void applyImmersive(Activity activity) {
        if (activity == null) return;
        try {
            Window window = activity.getWindow();
            if (window == null) return;

            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                    | WindowManager.LayoutParams.FLAG_FULLSCREEN);

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                WindowManager.LayoutParams lp = window.getAttributes();
                lp.layoutInDisplayCutoutMode = (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R)
                        ? WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
                        : WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
                window.setAttributes(lp);
            }

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                window.setDecorFitsSystemWindows(false);
                WindowInsetsController ctrl = window.getInsetsController();
                if (ctrl != null) {
                    ctrl.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
                    ctrl.setSystemBarsBehavior(
                            WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                }
            } else {
                View decor = window.getDecorView();
                if (decor != null) {
                    decor.setSystemUiVisibility(
                            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                                    | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                                    | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                                    | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "applyImmersive failed", t);
        }
    }

    public static String getSafeUserId(Context context) {
        if (context == null) return "00000000-0000-0000-0000-000000000000";
        try {
            SharedPreferences prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
            String cached = prefs.getString(KEY_USER_ID, null);
            if (cached != null && !cached.isEmpty()) {
                return cached;
            }
            String androidId = Settings.Secure.getString(
                    context.getContentResolver(), Settings.Secure.ANDROID_ID);
            UUID uuid;
            if (androidId != null && !androidId.isEmpty()) {
                long msb = ((long) androidId.hashCode() << 32) | (androidId.hashCode() & 0xffffffffL);
                long lsb = ((long) context.getPackageName().hashCode() << 32) | 0x4744313032L;
                uuid = new UUID(msb, lsb);
            } else {
                uuid = UUID.randomUUID();
            }
            String value = uuid.toString();
            prefs.edit().putString(KEY_USER_ID, value).apply();
            return value;
        } catch (Throwable t) {
            Log.w(TAG, "getSafeUserId fallback", t);
            return UUID.randomUUID().toString();
        }
    }

    public static boolean isNetworkAvailable(Context context) {
        // The bundled private 1.0 GDPS server answers entirely in-process over loopback,
        // so online level browsing must keep working with Wi-Fi/mobile data switched off.
        if (sLocalGdps) return true;
        if (context == null) return false;
        try {
            ConnectivityManager cm = (ConnectivityManager)
                    context.getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm == null) return false;
            Network active = cm.getActiveNetwork();
            if (active == null) return false;
            NetworkCapabilities caps = cm.getNetworkCapabilities(active);
            return caps != null && caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        } catch (Throwable t) {
            return false;
        }
    }

    public static Intent registerScreenReceiver(
            Context context, BroadcastReceiver receiver, IntentFilter filter) {
        if (context == null || receiver == null || filter == null) return null;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return context.registerReceiver(receiver, filter, Context.RECEIVER_EXPORTED);
        }
        return context.registerReceiver(receiver, filter);
    }

    public static void safeOpenUrl(Context context, String url) {
        if (context == null || url == null || url.isEmpty()) return;
        try {
            String normalized = url.trim();
            if (!normalized.contains("://")) {
                normalized = "https://" + normalized;
            }
            Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(normalized));
            if (!(context instanceof Activity)) {
                intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            }
            context.startActivity(intent);
        } catch (Throwable t) {
            Log.w(TAG, "safeOpenUrl ignored error for " + url, t);
        }
    }

    public static byte[] downloadUrlBytes(String urlString) {
        if (urlString == null || urlString.isEmpty()) return null;
        HttpURLConnection conn = null;
        try {
            URL url = new URL(urlString);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(10000);
            conn.setReadTimeout(15000);
            conn.setInstanceFollowRedirects(true);
            try (InputStream in = conn.getInputStream();
                 ByteArrayOutputStream out = new ByteArrayOutputStream()) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) != -1) {
                    out.write(buf, 0, n);
                }
                return out.toByteArray();
            }
        } catch (Throwable t) {
            Log.w(TAG, "downloadUrlBytes failed for " + urlString, t);
            return null;
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}
