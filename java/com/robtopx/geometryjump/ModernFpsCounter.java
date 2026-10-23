// SPDX-License-Identifier: MIT
package com.robtopx.geometryjump;

import android.app.Activity;
import android.graphics.Typeface;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.lang.ref.WeakReference;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Small always-on FPS read-out in the top-left corner of the screen (port 1.1.1).
 *
 * <p>The frames are counted where the engine actually draws them: {@code Cocos2dxRenderer.onDrawFrame}
 * calls {@link ModernPlatformHelper#onFrameRendered()} once per rendered frame, which bumps
 * {@link #onFrame()}. A 500 ms sample window turns that counter into the familiar one-decimal reading,
 * so the number reflects real frames the guest produced — not vsync callbacks, and not the display
 * refresh rate — which is exactly what is interesting on emulators and low-end devices.
 *
 * <p>The label deliberately uses a plain Android {@code sans-serif} face (the game's own labels are
 * rendered from the Pusab bitmap font by the native engine), a small 11sp size, and is drawn as a
 * normal {@link TextView} on top of the GL surface, so it costs the game nothing beyond one composited
 * layer that only repaints twice per second. Padding follows the display cutout so it stays visible
 * on notched hardware in immersive fullscreen.
 */
public final class ModernFpsCounter {
    private static final String TAG = "gd102-fps";
    private static final long SAMPLE_MS = 500L;
    private static final float TEXT_SIZE_SP = 11f;
    private static final int MARGIN_LEFT_DP = 10;
    private static final int MARGIN_TOP_DP = 8;

    /** Frames rendered since the last sample; written by the GL thread, drained by the UI thread. */
    private static final AtomicInteger sFrames = new AtomicInteger();

    private static volatile TextView sView;
    private static volatile Handler sHandler;
    private static volatile long sWindowStartNanos;
    private static volatile boolean sInstalled;
    private static WeakReference<Activity> sActivity;

    private static final Runnable TICK = new Runnable() {
        @Override
        public void run() {
            TextView view = sView;
            Handler handler = sHandler;
            if (view == null || handler == null) return;

            long now = System.nanoTime();
            int frames = sFrames.getAndSet(0);
            long start = sWindowStartNanos;
            sWindowStartNanos = now;
            long elapsed = now - start;
            if (elapsed > 0L) {
                double fps = frames * 1_000_000_000.0 / (double) elapsed;
                if (fps < 0.05) fps = 0.0;
                view.setText(String.format(Locale.US, "%.1f", fps));
            }
            handler.postDelayed(this, SAMPLE_MS);
        }
    };

    private ModernFpsCounter() {}

    /** Called once per rendered frame from the GL thread. Must stay allocation-free. */
    public static void onFrame() {
        sFrames.incrementAndGet();
    }

    /** Builds the overlay for {@code activity}; safe to call more than once. */
    public static void install(final Activity activity) {
        if (activity == null || sInstalled) return;
        sInstalled = true;
        sActivity = new WeakReference<>(activity);
        try {
            final View decor = activity.getWindow().getDecorView();
            // Post instead of attaching immediately: initBridge() runs before super.onCreate(), and the
            // decor's content is installed later in the same call.
            decor.post(new Runnable() {
                @Override
                public void run() {
                    attach(activity);
                }
            });
        } catch (Throwable t) {
            Log.w(TAG, "FPS counter install failed", t);
        }
    }

    private static void attach(Activity activity) {
        if (sView != null) return;
        try {
            View decor = activity.getWindow().getDecorView();
            if (!(decor instanceof ViewGroup)) {
                Log.w(TAG, "FPS counter: decor view is not a ViewGroup");
                return;
            }

            TextView view = new TextView(activity);
            view.setTextSize(TypedValue.COMPLEX_UNIT_SP, TEXT_SIZE_SP);
            view.setTypeface(Typeface.create("sans-serif", Typeface.NORMAL));
            view.setTextColor(0xFFFFFFFF);
            view.setShadowLayer(3f, 1f, 1f, 0xE6000000);
            view.setIncludeFontPadding(false);
            view.setText("--.-");

            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    Gravity.TOP | Gravity.START);
            ((ViewGroup) decor).addView(view, lp);
            applyInsets(view, activity);

            sView = view;
            sHandler = new Handler(Looper.getMainLooper());
            sWindowStartNanos = System.nanoTime();
            sHandler.postDelayed(TICK, SAMPLE_MS);
        } catch (Throwable t) {
            Log.w(TAG, "FPS counter attach failed", t);
        }
    }

    private static void applyInsets(final TextView view, final Activity activity) {
        try {
            view.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
                @Override
                public WindowInsets onApplyWindowInsets(View v, WindowInsets insets) {
                    int left = 0;
                    int top = 0;
                    if (Build.VERSION.SDK_INT >= 30) {
                        android.graphics.Insets cut = insets.getInsets(WindowInsets.Type.displayCutout());
                        left = cut.left;
                        top = cut.top;
                    } else {
                        left = insets.getSystemWindowInsetLeft();
                        top = insets.getSystemWindowInsetTop();
                        if (Build.VERSION.SDK_INT >= 28) {
                            DisplayCutout cutout = insets.getDisplayCutout();
                            if (cutout != null) {
                                left = Math.max(left, cutout.getSafeInsetLeft());
                                top = Math.max(top, cutout.getSafeInsetTop());
                            }
                        }
                    }
                    v.setPadding(left + dp(activity, MARGIN_LEFT_DP),
                            top + dp(activity, MARGIN_TOP_DP),
                            dp(activity, MARGIN_LEFT_DP),
                            dp(activity, MARGIN_TOP_DP));
                    return insets;
                }
            });
            view.requestApplyInsets();
        } catch (Throwable t) {
            Log.w(TAG, "FPS counter insets failed", t);
        }
    }

    private static int dp(Activity activity, int value) {
        try {
            return Math.round(value * activity.getResources().getDisplayMetrics().density);
        } catch (Throwable t) {
            return value;
        }
    }

    /** The activity that currently owns the overlay, or {@code null}. */
    public static Activity currentActivity() {
        return sActivity == null ? null : sActivity.get();
    }
}
