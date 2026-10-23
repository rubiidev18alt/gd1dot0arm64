// SPDX-License-Identifier: MIT
package com.robtopx.geometryjump;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

/**
 * Modern Android (API 24-36 / Android 7-16) replacement for Geometry Dash 1.02's
 * obfuscated Cocos2d-x 2.1.4 {@code Cocos2dxMusic} ({@code org.cocos2dx.lib.p}).
 *
 * <p>Why the original 2013 {@code org.cocos2dx.lib.p} restarts music from 0:00 on unpause:
 * <ol>
 *   <li>When unpausing mid-level ({@code PlayLayer::onResume}), GD 1.02 sees
 *       {@code !isBackgroundMusicPlaying()} and calls back-to-back:
 *       {@code playBackgroundMusic(song, false)} -> {@code setBackgroundMusicTime(sec)} ->
 *       {@code resumeBackgroundMusic()}.</li>
 *   <li>Original {@code p.playBackgroundMusic} calls {@code stop(); prepare(); seekTo(0); start();},
 *       which in modern Android {@code NuPlayerDriver} posts an asynchronous
 *       {@code seekToAsync(0, needNotify=true)}.</li>
 *   <li>Microseconds later, {@code p.setBackgroundMusicTime} calls {@code stop(); prepare();} again.
 *       In {@code NuPlayerDriver::prepare_l(STATE_STOPPED)}, {@code prepare()} posts a second
 *       {@code seekToAsync(0)} and waits on a condition variable that is woken prematurely by the
 *       first {@code seekTo(0)}'s completion notification, causing the queued {@code seekToAsync(0)}
 *       (and/or compressed offload teardown) to reset playback back to 0:00.</li>
 * </ol>
 *
 * <p>This controller never calls {@code stop() + prepare()} on an active {@link MediaPlayer},
 * preserves the exact paused decoder state across {@code playBackgroundMusic -> setBackgroundMusicTime
 * -> resumeBackgroundMusic}, uses {@link AudioAttributes#USAGE_GAME}, closes every
 * {@link AssetFileDescriptor}, and uses {@link MediaPlayer#SEEK_CLOSEST} on API 26+.
 */
public final class ModernMusicController {
    private static final String TAG = "gd102-music";
    private static final Object LOCK = new Object();

    // Maximum drift (ms) between paused MediaPlayer position and GD's x/311.58s target
    // within which we resume the paused decoder directly with zero seeks/flushes.
    private static final int PAUSE_SYNC_TOLERANCE_MS = 450;

    // After ~2.5s paused on some OEM audio HALs, offloaded streams may tear down and need
    // an explicit seek confirmation upon resume.
    private static final long PAUSE_RECONFIRM_SEEK_MS = 2500L;

    // Fallback window (ms) for the rare case where playBackgroundMusic(sameTrack) is called
    // while paused WITHOUT a subsequent setBackgroundMusicTime/resumeBackgroundMusic (e.g. fullReset at x=0).
    private static final long PENDING_ZERO_RESTART_DELAY_MS = 25L;

    private static Context sAppContext = null;
    private static MediaPlayer sPlayer = null;
    private static String sCurrentPath = null;
    private static boolean sLooping = false;
    private static boolean sPaused = false;
    private static boolean sStarted = false;
    private static int sPausedPosMs = 0;
    private static long sPauseUptimeMs = 0L;
    private static float sLeftVolume = 0.5f;
    private static float sRightVolume = 0.5f;
    private static long sOpToken = 0L;
    private static boolean sPendingZeroRestart = false;

    private ModernMusicController() {}

    public static void init(Context context) {
        synchronized (LOCK) {
            if (context != null) {
                Context app = context.getApplicationContext();
                sAppContext = (app != null) ? app : context;
            }
        }
    }

    public static void onHolderConstructed(Context context) {
        synchronized (LOCK) {
            if (context != null) {
                Context app = context.getApplicationContext();
                sAppContext = (app != null) ? app : context;
            }
            cancelPendingZeroRestartLocked();
            releasePlayerLocked();
            sCurrentPath = null;
            sLooping = false;
            sLeftVolume = 0.5f;
            sRightVolume = 0.5f;
        }
    }

    public static void preload(Context context, String path) {
        if (path == null || path.isEmpty()) return;
        synchronized (LOCK) {
            updateContextLocked(context);
            if (sPlayer == null || !path.equals(sCurrentPath)) {
                cancelPendingZeroRestartLocked();
                releasePlayerLocked();
                sPlayer = createPreparedPlayerLocked(path);
                sCurrentPath = (sPlayer != null) ? path : null;
            }
        }
    }

    public static void play(Context context, final String path, final boolean loop) {
        if (path == null || path.isEmpty()) return;
        synchronized (LOCK) {
            updateContextLocked(context);
            cancelPendingZeroRestartLocked();

            // Critical unpause path: when paused on the exact same track, GD 1.02 calls
            // playBackgroundMusic(song, false) immediately before setBackgroundMusicTime(bgTime)
            // and resumeBackgroundMusic(). Keep the paused MediaPlayer intact at sPausedPosMs
            // instead of calling stop()/prepare()/seekTo(0)!
            if (sPlayer != null && path.equals(sCurrentPath) && sPaused) {
                sLooping = loop;
                try {
                    sPlayer.setLooping(loop);
                } catch (Throwable t) {
                    Log.w(TAG, "setLooping while paused failed", t);
                }
                scheduleZeroRestartFallbackLocked(path, loop);
                return;
            }

            // Starting from 0:00 (new track, stopped player, or restarting an already-started track).
            if (sPlayer != null && (!path.equals(sCurrentPath) || sStarted)) {
                releasePlayerLocked();
            }
            if (sPlayer == null) {
                sPlayer = createPreparedPlayerLocked(path);
                sCurrentPath = (sPlayer != null) ? path : null;
            }
            if (sPlayer == null) {
                Log.e(TAG, "play: failed to prepare MediaPlayer for " + path);
                return;
            }

            try {
                sLooping = loop;
                sPlayer.setLooping(loop);
                sPlayer.start();
                sStarted = true;
                sPaused = false;
                sPausedPosMs = 0;
            } catch (Throwable t) {
                Log.e(TAG, "play failed for " + path, t);
            }
        }
    }

    public static void setTime(float seconds) {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();

            if (sPlayer == null && sCurrentPath != null) {
                sPlayer = createPreparedPlayerLocked(sCurrentPath);
                if (sPlayer != null) {
                    try {
                        sPlayer.setLooping(sLooping);
                    } catch (Throwable ignored) {}
                }
            }
            if (sPlayer == null) return;

            int targetMs = Math.max(0, Math.round(seconds * 1000.0f));
            try {
                int durationMs = sPlayer.getDuration();
                if (durationMs > 0 && targetMs >= durationMs) {
                    targetMs = sLooping ? (targetMs % durationMs) : Math.max(0, durationMs - 1);
                }
            } catch (Throwable ignored) {}

            try {
                long pauseElapsedMs = SystemClock.uptimeMillis() - sPauseUptimeMs;
                int curPosMs = Math.max(0, sPlayer.getCurrentPosition());

                if (sPaused
                        && pauseElapsedMs < PAUSE_RECONFIRM_SEEK_MS
                        && Math.abs(targetMs - sPausedPosMs) <= PAUSE_SYNC_TOLERANCE_MS
                        && Math.abs(targetMs - curPosMs) <= PAUSE_SYNC_TOLERANCE_MS) {
                    // Already paused at the exact target PCM frame: resume cleanly with zero seeks!
                    sPlayer.start();
                    sStarted = true;
                    sPaused = false;
                    return;
                }

                // Ensure NuPlayer is in STARTED state BEFORE issuing seekTo so AOSP NuPlayer's
                // !mStarted audio-only early-return path is never triggered.
                if (!sStarted || !sPlayer.isPlaying()) {
                    sPlayer.start();
                    sStarted = true;
                }
                seekToMsLocked(sPlayer, targetMs);
                sPaused = false;
                sPausedPosMs = targetMs;
            } catch (Throwable t) {
                Log.e(TAG, "setTime(" + seconds + ") failed", t);
            }
        }
    }

    public static float getTime() {
        synchronized (LOCK) {
            if (sPlayer == null) return 0.0f;
            try {
                int posMs = sPaused ? sPausedPosMs : sPlayer.getCurrentPosition();
                return Math.max(0, posMs) / 1000.0f;
            } catch (Throwable t) {
                return 0.0f;
            }
        }
    }

    public static void pause() {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();
            if (sPlayer == null) return;
            try {
                if (sPlayer.isPlaying()) {
                    sPausedPosMs = Math.max(0, sPlayer.getCurrentPosition());
                    sPauseUptimeMs = SystemClock.uptimeMillis();
                    sPlayer.pause();
                    sPaused = true;
                }
            } catch (Throwable t) {
                Log.w(TAG, "pause failed", t);
            }
        }
    }

    public static void resume() {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();
            if (sPlayer == null || !sPaused) return;
            try {
                long pauseElapsedMs = SystemClock.uptimeMillis() - sPauseUptimeMs;
                int curPosMs = Math.max(0, sPlayer.getCurrentPosition());
                sPlayer.start();
                sStarted = true;
                if (sPausedPosMs > 0
                        && (pauseElapsedMs >= PAUSE_RECONFIRM_SEEK_MS
                                || Math.abs(curPosMs - sPausedPosMs) > PAUSE_SYNC_TOLERANCE_MS)) {
                    seekToMsLocked(sPlayer, sPausedPosMs);
                }
                sPaused = false;
            } catch (Throwable t) {
                Log.w(TAG, "resume failed", t);
            }
        }
    }

    public static void rewind() {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();
            if (sCurrentPath == null || sCurrentPath.isEmpty()) return;
            String path = sCurrentPath;
            boolean loop = sLooping;
            releasePlayerLocked();
            sPlayer = createPreparedPlayerLocked(path);
            sCurrentPath = (sPlayer != null) ? path : null;
            if (sPlayer != null) {
                try {
                    sPlayer.setLooping(loop);
                    sPlayer.start();
                    sStarted = true;
                    sPaused = false;
                    sPausedPosMs = 0;
                } catch (Throwable t) {
                    Log.w(TAG, "rewind failed", t);
                }
            }
        }
    }

    public static void stop() {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();
            releasePlayerLocked();
        }
    }

    public static void end() {
        synchronized (LOCK) {
            cancelPendingZeroRestartLocked();
            releasePlayerLocked();
            sCurrentPath = null;
            sLooping = false;
            sLeftVolume = 0.5f;
            sRightVolume = 0.5f;
        }
    }

    public static boolean isPlaying() {
        synchronized (LOCK) {
            if (sPlayer == null || sPaused) return false;
            try {
                return sPlayer.isPlaying();
            } catch (Throwable t) {
                return false;
            }
        }
    }

    public static float getVolume() {
        synchronized (LOCK) {
            if (sPlayer == null) return 0.0f;
            return (sLeftVolume + sRightVolume) * 0.5f;
        }
    }

    public static void setVolume(float volume) {
        float clamped = volume;
        if (clamped < 0.0f) clamped = 0.0f;
        if (clamped > 1.0f) clamped = 1.0f;
        synchronized (LOCK) {
            sLeftVolume = clamped;
            sRightVolume = clamped;
            if (sPlayer != null) {
                try {
                    sPlayer.setVolume(sLeftVolume, sRightVolume);
                } catch (Throwable t) {
                    Log.w(TAG, "setVolume failed", t);
                }
            }
        }
    }

    private static void updateContextLocked(Context context) {
        if (context != null) {
            Context app = context.getApplicationContext();
            sAppContext = (app != null) ? app : context;
        }
    }

    private static void cancelPendingZeroRestartLocked() {
        sOpToken++;
        sPendingZeroRestart = false;
    }

    private static void scheduleZeroRestartFallbackLocked(final String path, final boolean loop) {
        final long token = ++sOpToken;
        sPendingZeroRestart = true;
        Looper mainLooper = Looper.getMainLooper();
        if (mainLooper == null) {
            return;
        }
        new Handler(mainLooper).postDelayed(new Runnable() {
            @Override
            public void run() {
                synchronized (LOCK) {
                    if (token != sOpToken || !sPendingZeroRestart) {
                        return;
                    }
                    sPendingZeroRestart = false;
                    releasePlayerLocked();
                    sPlayer = createPreparedPlayerLocked(path);
                    sCurrentPath = (sPlayer != null) ? path : null;
                    if (sPlayer != null) {
                        try {
                            sLooping = loop;
                            sPlayer.setLooping(loop);
                            sPlayer.start();
                            sStarted = true;
                            sPaused = false;
                            sPausedPosMs = 0;
                        } catch (Throwable t) {
                            Log.w(TAG, "deferred zero-restart failed", t);
                        }
                    }
                }
            }
        }, PENDING_ZERO_RESTART_DELAY_MS);
    }

    @SuppressWarnings("deprecation")
    private static void seekToMsLocked(MediaPlayer mp, int targetMs) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            mp.seekTo((long) targetMs, MediaPlayer.SEEK_CLOSEST);
        } else {
            mp.seekTo(targetMs);
        }
    }

    private static void releasePlayerLocked() {
        if (sPlayer != null) {
            try {
                sPlayer.release();
            } catch (Throwable ignored) {}
            sPlayer = null;
        }
        sPaused = false;
        sStarted = false;
        sPausedPosMs = 0;
    }

    private static MediaPlayer createPreparedPlayerLocked(String path) {
        if (path == null || path.isEmpty()) return null;
        MediaPlayer mp = new MediaPlayer();
        AssetFileDescriptor afd = null;
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.LOLLIPOP) {
                mp.setAudioAttributes(new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build());
            }
            if (path.startsWith("/")) {
                mp.setDataSource(path);
            } else {
                if (sAppContext == null) {
                    Log.e(TAG, "createPreparedPlayerLocked: sAppContext is null for " + path);
                    mp.release();
                    return null;
                }
                afd = sAppContext.getAssets().openFd(path);
                mp.setDataSource(afd.getFileDescriptor(), afd.getStartOffset(), afd.getLength());
            }
            mp.prepare();
            mp.setVolume(sLeftVolume, sRightVolume);
            sPaused = false;
            sStarted = false;
            sPausedPosMs = 0;
            return mp;
        } catch (Throwable t) {
            Log.e(TAG, "createPreparedPlayerLocked failed for " + path, t);
            try {
                mp.release();
            } catch (Throwable ignored) {}
            return null;
        } finally {
            if (afd != null) {
                try {
                    afd.close();
                } catch (Throwable ignored) {}
            }
        }
    }
}
