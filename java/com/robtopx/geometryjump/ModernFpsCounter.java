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
 * Small always-on FPS read-out in the top-left corner of the screen (port 1.1.1, attachment made
 * self-healing in 1.1.2 after early installs could silently vanish).
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
 *
 * <p>Attachment strategy (1.1.2): {@link #install} never touches the view hierarchy itself — it only
 * stores the activity and starts the sample ticker. The ticker (main looper, first tick half a second
 * after install, i.e. long after {@code onCreate} has finished) performs the attach, and re-checks on
 * every tick whether the label still has a parent. This survives every window ordering we have seen:
 * {@code initBridge} runs before {@code super.onCreate}, later {@code setContentView} calls clear the
 * content parent (the label lives directly on the decor view, not the content parent), and activity
 * recreation swaps the window underneath us. Failures are logged once per attempt under {@code gd102-fps}
 * so a device that still refuses the overlay leaves evidence in logcat.
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
    private static volatile WeakReference<Activity> sActivity = new WeakReference<>(null);
    private static volatile boolean sAttachWarned;
    private static volatile boolean sHookSilentWarned;

    private static int sSilentTicks;

    private static final Runnable TICK = new Runnable() {
        @Override
        public void run() {
            Handler handler = sHandler;
            if (handler == null) return;

            ensureAttached();
            updateText();
            handler.postDelayed(this, SAMPLE_MS);
        }
    };

    private ModernFpsCounter() {}

    /** Called once per rendered frame from the GL thread. Must stay allocation-free. */
    public static void onFrame() {
        sFrames.incrementAndGet();
    }

    /**
     * Records the activity that owns the overlay and starts the self-healing sample ticker.
     * Deliberately view-free: this is called from {@code initBridge}, which runs before
     * {@code super.onCreate()}, so no window/view work is safe here yet. Called more than once
     * (activity recreation) simply re-targets the overlay.
     */
    public static void install(final Activity activity) {
        if (activity == null) return;
        sActivity = new WeakReference<>(activity);
        try {
            Handler handler = sHandler;
            if (handler == null) {
                handler = new Handler(Looper.getMainLooper());
                sHandler = handler;
                sWindowStartNanos = System.nanoTime();
                handler.postDelayed(TICK, SAMPLE_MS);
            }
        } catch (Throwable t) {
            Log.w(TAG, "FPS counter install failed", t);
        }
    }

    /**
     * Makes sure the label exists and is attached to the current window's decor view. Runs on the
     * main looper (ticker). Safe to call repeatedly; each check is a pointer comparison.
     */
    public static void ensureAttached() {
        Activity activity = sActivity.get();
        if (activity == null) return;
        try {
            View existing = sView;
            if (existing != null && existing.getParent() != null) {
                // Still attached; also re-target if a new activity replaced the window.
                Activity owner = (existing.getContext() instanceof Activity)
                        ? (Activity) existing.getContext() : null;
                if (owner == activity) return;
                detach();
            }
            attach(activity);
        } catch (Throwable t) {
            if (!sAttachWarned) {
                sAttachWarned = true;
                Log.w(TAG, "FPS counter attach failed", t);
            }
        }
    }

    private static void detach() {
        View existing = sView;
        sView = null;
        if (existing != null && existing.getParent() instanceof ViewGroup) {
            try {
                ((ViewGroup) existing.getParent()).removeView(existing);
            } catch (Throwable ignored) {
            }
        }
    }

    private static void attach(Activity activity) {
        View decor = activity.getWindow().getDecorView();
        if (!(decor instanceof ViewGroup)) {
            if (!sAttachWarned) {
                sAttachWarned = true;
                Log.w(TAG, "FPS counter: decor view is not a ViewGroup (" + decor + ")");
            }
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
        try {
            // Direct child of the decor view: setContentView() clears the content parent only, so
            // the label cannot be wiped by later window setup, and it draws above the GL surface.
            ((ViewGroup) decor).addView(view, lp);
        } catch (Throwable decorFailed) {
            // Fallback: activity content parent (still above the game view when added last).
            activity.addContentView(view, lp);
        }

        applyInsets(view, activity);
        sView = view;
        sWindowStartNanos = System.nanoTime();
        sAttachWarned = false;
        Log.i(TAG, "FPS overlay attached to decor of " + activity.getClass().getSimpleName());
    }

    private static void updateText() {
        TextView view = sView;
        if (view == null) return;

        long now = System.nanoTime();
        int frames = sFrames.getAndSet(0);
        long start = sWindowStartNanos;
        sWindowStartNanos = now;
        long elapsed = now - start;
        if (elapsed > 0L) {
            double fps = frames * 1_000_000_000.0 / (double) elapsed;
            if (fps < 0.05) fps = 0.0;
            view.setText(String.format(Locale.US, "%.1f", fps));
            if (frames == 0) {
                // The onDrawFrame hook should be counting; if it never does, say so once so a
                // broken smali hook is distinguishable from a broken overlay.
                if (++sSilentTicks >= 4 && !sHookSilentWarned) {
                    sHookSilentWarned = true;
                    Log.w(TAG, "No frames counted for 2s — ModernPlatformHelper.onFrameRendered() "
                            + "hook is not firing (overlay itself is attached)");
                }
            } else {
                sSilentTicks = 0;
            }
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
        return sActivity.get();
    }
}
