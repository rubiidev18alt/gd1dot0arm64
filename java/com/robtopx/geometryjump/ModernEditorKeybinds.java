// SPDX-License-Identifier: MIT
package com.robtopx.geometryjump;

import android.app.Activity;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.view.AccessibilityEvent;

import java.util.concurrent.atomic.AtomicInteger;

/**
 * PC editor keybinds for the Geometry Dash 1.02 / 1.1 editors (port 1.1.2), mirroring the
 * official desktop client:
 *
 * <ul>
 *   <li><b>Shift (held)</b> — swipe is active while the key is down, exactly like holding Shift
 *       on the Steam build; releasing restores the previous (button) state.</li>
 *   <li><b>T</b> — toggle swipe.</li>
 *   <li><b>Scroll wheel</b> / <b>+ −</b> (also numpad +/-) — zoom out/in through the editor's own
 *       {@code EditorUI::zoomOut/zoomIn} steps.</li>
 *   <li><b>Ctrl+Z</b> — undo; <b>Ctrl+Shift+Z</b> / <b>Ctrl+Y</b> — redo.</li>
 *   <li><b>Arrow keys</b> — pan the editor view (viewport follows the arrow).</li>
 * </ul>
 *
 * <p>How it works: {@link #install} wraps the activity's {@link Window.Callback}, so every key and
 * scroll event is visible before any view sees it. Handlers only accumulate intent into atomics;
 * {@link #onFrameGlThread()} drains them on the GL thread between frames, where each request is
 * handed to the native editor bridge. The bridge re-checks the live scene graph and no-ops
 * unless a LevelEditorLayer with a valid EditorUI is actually running — so nothing here can fire
 * during gameplay or in menus. While a text input has focus (level name), or when the window
 * loses focus, all events pass through untouched.
 */
public final class ModernEditorKeybinds {
    private static final String TAG = "gd102-keybinds";
    /** Pan distance per arrow-key step, in editor world units (tunable). */
    private static final int PAN_STEP = 50;

    private static volatile Activity sActivity;
    private static ModernEditorKeybinds sWrapper;

    private static volatile boolean sShift;
    private static volatile boolean sCtrl;
    private static volatile boolean sShiftSwipeArmed;
    /** Updated on the GL thread from the native probe; read on the input thread. */
    private static volatile boolean sInEditor;
    private static volatile boolean sBroken;

    // Pending work, accumulated on the input thread and drained on the GL thread.
    private static final AtomicInteger sZoom = new AtomicInteger();      // >0 zoom in, <0 zoom out
    private static final AtomicInteger sPanX = new AtomicInteger();
    private static final AtomicInteger sPanY = new AtomicInteger();
    private static final AtomicInteger sUndo = new AtomicInteger();
    private static final AtomicInteger sRedo = new AtomicInteger();
    private static final AtomicInteger sToggles = new AtomicInteger();
    private static final AtomicInteger sSwipeReq = new AtomicInteger(-1);  // -1 none, 0/1 request

    private ModernEditorKeybinds(Window.Callback delegate) {
        mDelegate = delegate;
    }

    private final Window.Callback mDelegate;

    /** Wraps the activity's window callback. Safe to call before {@code super.onCreate()}. */
    public static void install(Activity activity) {
        if (activity == null) return;
        sActivity = activity;
        try {
            View decor = activity.getWindow().getDecorView();
            decor.post(new Runnable() {
                @Override
                public void run() {
                    try {
                        Window window = activity.getWindow();
                        Window.Callback cb = window.getCallback();
                        if (cb == sWrapper) return;
                        ModernEditorKeybinds wrapped = new ModernEditorKeybinds(
                                cb != null ? cb : new NullCallback());
                        window.setCallback(wrapped);
                        sWrapper = wrapped;
                        Log.i(TAG, "Editor keybind window callback installed");
                    } catch (Throwable t) {
                        Log.w(TAG, "Failed to install editor keybind callback", t);
                    }
                }
            });
        } catch (Throwable t) {
            Log.w(TAG, "Editor keybind install failed", t);
        }
    }

    /**
     * GL-thread tick (from {@link ModernPlatformHelper#onFrameRendered()}): refresh the editor
     * probe, then execute everything queued by the input thread while the guest is between frames.
     */
    public static void onFrameGlThread() {
        if (sBroken) return;
        try {
            sInEditor = ModernPlatformHelper.nativeEditorProbe();
            if (!sInEditor) {
                // Drop stale intents from a scene we have already left (except a pending swipe
                // restore, which the bridge itself gates by re-probing).
                sZoom.getAndSet(0);
                sPanX.getAndSet(0);
                sPanY.getAndSet(0);
                sUndo.getAndSet(0);
                sRedo.getAndSet(0);
                sToggles.getAndSet(0);
            }
            int z = sZoom.getAndSet(0);
            if (z != 0) ModernPlatformHelper.nativeEditorZoom(z);
            int px = sPanX.getAndSet(0);
            int py = sPanY.getAndSet(0);
            if (px != 0 || py != 0) ModernPlatformHelper.nativeEditorPan(px, py);
            int u = sUndo.getAndSet(0);
            if (u > 0) ModernPlatformHelper.nativeEditorUndo(u);
            int r = sRedo.getAndSet(0);
            if (r > 0) ModernPlatformHelper.nativeEditorRedo(r);
            int t = sToggles.getAndSet(0);
            if (t > 0) {
                int n = Math.min(t, 4);
                for (int i = 0; i < n; i++) ModernPlatformHelper.nativeEditorToggleSwipe();
            }
            int sw = sSwipeReq.getAndSet(-1);
            if (sw >= 0) ModernPlatformHelper.nativeEditorSwipe(sw == 1);
        } catch (UnsatisfiedLinkError ule) {
            sBroken = true;
            Log.w(TAG, "Editor keybind natives unavailable; keybinds disabled", ule);
        } catch (Throwable t) {
            Log.w(TAG, "Editor keybind frame tick failed", t);
        }
    }

    private static boolean textInputFocused() {
        try {
            Activity a = sActivity;
            if (a == null) return false;
            View v = a.getCurrentFocus();
            if (v == null) return false;
            String name = v.getClass().getName();
            return name.contains("EditText") || name.contains("TextInput") || name.contains("EditBox");
        } catch (Throwable t) {
            return true;  // unsure -> pass keys through instead of hijacking them
        }
    }

    // ------------------------------------------------------------------
    // Key handling
    // ------------------------------------------------------------------

    private boolean handleKey(KeyEvent e) {
        int action = e.getAction();
        int code = e.getKeyCode();

        // Modifier tracking always runs so state never gets stuck.
        if (code == KeyEvent.KEYCODE_SHIFT_LEFT || code == KeyEvent.KEYCODE_SHIFT_RIGHT) {
            boolean down = action == KeyEvent.ACTION_DOWN;
            sShift = down;
            if (down && e.getRepeatCount() == 0 && sInEditor && !textInputFocused()) {
                sShiftSwipeArmed = true;
                sSwipeReq.set(1);
            } else if (!down && sShiftSwipeArmed) {
                sShiftSwipeArmed = false;
                sSwipeReq.set(0);
            }
            return false;  // modifiers themselves are never consumed
        }
        if (code == KeyEvent.KEYCODE_CTRL_LEFT || code == KeyEvent.KEYCODE_CTRL_RIGHT
                || code == KeyEvent.KEYCODE_META_LEFT || code == KeyEvent.KEYCODE_META_RIGHT) {
            sCtrl = action == KeyEvent.ACTION_DOWN;
            return false;
        }

        boolean inEditor = sInEditor && !textInputFocused();
        if (!inEditor) return false;

        if (action != KeyEvent.ACTION_DOWN) {
            // Swallow key-ups for keys we acted on (otherwise nothing depends on it; the game
            // ignores non-back keys, so simply forward everything on UP to keep it simple).
            return false;
        }

        boolean ctrl = sCtrl;
        boolean shift = sShift;
        switch (code) {
            case KeyEvent.KEYCODE_T:
                if (!ctrl) {
                    sToggles.incrementAndGet();
                    return true;
                }
                return false;
            case KeyEvent.KEYCODE_Z:
                if (ctrl) {
                    if (shift) sRedo.incrementAndGet();
                    else sUndo.incrementAndGet();
                    return true;
                }
                return false;
            case KeyEvent.KEYCODE_Y:
                if (ctrl) {
                    sRedo.incrementAndGet();
                    return true;
                }
                return false;
            case KeyEvent.KEYCODE_PLUS:
            case KeyEvent.KEYCODE_EQUALS:
            case KeyEvent.KEYCODE_NUMPAD_ADD:
                sZoom.incrementAndGet();
                return true;
            case KeyEvent.KEYCODE_MINUS:
            case KeyEvent.KEYCODE_NUMPAD_SUBTRACT:
                sZoom.decrementAndGet();
                return true;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                sPanX.addAndGet(+PAN_STEP);
                return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                sPanX.addAndGet(-PAN_STEP);
                return true;
            case KeyEvent.KEYCODE_DPAD_UP:
                sPanY.addAndGet(-PAN_STEP);
                return true;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                sPanY.addAndGet(+PAN_STEP);
                return true;
            default:
                return false;  // BACK, menu keys, letters... all pass through
        }
    }

    private boolean handleGenericMotion(MotionEvent e) {
        try {
            if (e.getActionMasked() == MotionEvent.ACTION_SCROLL
                    && sInEditor && !textInputFocused()) {
                float v = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
                int steps = Math.round(v);
                if (steps > 6) steps = 6;
                if (steps < -6) steps = -6;
                if (steps != 0) {
                    sZoom.addAndGet(steps);
                    return true;
                }
            }
        } catch (Throwable t) {
            Log.w(TAG, "scroll handling failed", t);
        }
        return false;
    }

    private void focusChanged(boolean hasFocus) {
        if (!hasFocus) {
            sShift = false;
            sCtrl = false;
            if (sShiftSwipeArmed) {
                sShiftSwipeArmed = false;
                sSwipeReq.set(0);
            }
        }
    }

    // ------------------------------------------------------------------
    // Window.Callback delegation (full interface; no event is swallowed except editor keybinds)
    // ------------------------------------------------------------------

    public boolean dispatchKeyEvent(KeyEvent event) {
        try {
            if (handleKey(event)) return true;
        } catch (Throwable t) {
            Log.w(TAG, "key handling failed", t);
        }
        return mDelegate.dispatchKeyEvent(event);
    }

    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        try {
            if (handleGenericMotion(event)) return true;
        } catch (Throwable t) {
            Log.w(TAG, "motion handling failed", t);
        }
        return mDelegate.dispatchGenericMotionEvent(event);
    }

    public void onWindowFocusChanged(boolean hasFocus) {
        focusChanged(hasFocus);
        mDelegate.onWindowFocusChanged(hasFocus);
    }

    public boolean dispatchKeyShortcutEvent(KeyEvent event) {
        return mDelegate.dispatchKeyShortcutEvent(event);
    }

    public boolean dispatchTouchEvent(MotionEvent event) {
        return mDelegate.dispatchTouchEvent(event);
    }

    public boolean dispatchTrackballEvent(MotionEvent event) {
        return mDelegate.dispatchTrackballEvent(event);
    }

    public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) {
        return mDelegate.dispatchPopulateAccessibilityEvent(event);
    }

    public View onCreatePanelView(int featureId) {
        return mDelegate.onCreatePanelView(featureId);
    }

    public boolean onCreatePanelMenu(int featureId, Menu menu) {
        return mDelegate.onCreatePanelMenu(featureId, menu);
    }

    public boolean onPreparePanel(int featureId, View view, Menu menu) {
        return mDelegate.onPreparePanel(featureId, view, menu);
    }

    public boolean onMenuOpened(int featureId, Menu menu) {
        return mDelegate.onMenuOpened(featureId, menu);
    }

    public boolean onMenuItemSelected(int featureId, MenuItem item) {
        return mDelegate.onMenuItemSelected(featureId, item);
    }

    public void onWindowAttributesChanged(WindowManager.LayoutParams attrs) {
        mDelegate.onWindowAttributesChanged(attrs);
    }

    public void onAttachedToWindow() {
        mDelegate.onAttachedToWindow();
    }

    public void onDetachedFromWindow() {
        mDelegate.onDetachedFromWindow();
    }

    public void onPanelClosed(int featureId, Menu menu) {
        mDelegate.onPanelClosed(featureId, menu);
    }

    public boolean onSearchRequested() {
        return mDelegate.onSearchRequested();
    }

    public boolean onSearchRequested(SearchEvent event) {
        return mDelegate.onSearchRequested(event);
    }

    public boolean onCapturedPointerEvent(MotionEvent event) {
        return mDelegate.onCapturedPointerEvent(event);
    }

    public void onPointerCaptureChanged(boolean hasCapture) {
        mDelegate.onPointerCaptureChanged(hasCapture);
    }

    /** Placeholder when the activity somehow has no window callback of its own. */
    private static final class NullCallback implements Window.Callback {
        public boolean dispatchKeyEvent(KeyEvent event) { return false; }
        public boolean dispatchKeyShortcutEvent(KeyEvent event) { return false; }
        public boolean dispatchTouchEvent(MotionEvent event) { return false; }
        public boolean dispatchTrackballEvent(MotionEvent event) { return false; }
        public boolean dispatchGenericMotionEvent(MotionEvent event) { return false; }
        public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent event) { return false; }
        public View onCreatePanelView(int featureId) { return null; }
        public boolean onCreatePanelMenu(int featureId, Menu menu) { return false; }
        public boolean onPreparePanel(int featureId, View view, Menu menu) { return false; }
        public boolean onMenuOpened(int featureId, Menu menu) { return false; }
        public boolean onMenuItemSelected(int featureId, MenuItem item) { return false; }
        public void onWindowAttributesChanged(WindowManager.LayoutParams attrs) { }
        public void onWindowFocusChanged(boolean hasFocus) { }
        public void onAttachedToWindow() { }
        public void onDetachedFromWindow() { }
        public void onPanelClosed(int featureId, Menu menu) { }
        public boolean onSearchRequested() { return false; }
        public boolean onSearchRequested(SearchEvent event) { return false; }
        public boolean onCapturedPointerEvent(MotionEvent event) { return false; }
        public void onPointerCaptureChanged(boolean hasCapture) { }
    }
}
