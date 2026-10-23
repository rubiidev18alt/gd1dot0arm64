package com.robtopx.geometryjump;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.opengl.GLES20;
import android.util.Log;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * Texture-size-aware replacement for the 2013 cocos2d-x 2.x text layout in
 * {@code org.cocos2dx.lib.Cocos2dxBitmap}.
 *
 * <p>The original implementation was written for 480x320 phones. On a modern tablet or a
 * high-density phone the engine asks for labels whose pixel size exceeds
 * {@code GL_MAX_TEXTURE_SIZE}, so the texture upload silently fails and the text simply
 * disappears; lines are also wrapped with an integer-only algorithm that mis-measures wide
 * glyphs and can drop the last word of a line. This class keeps the exact same Java/JNI
 * contract (the native side still calls
 * {@code Cocos2dxBitmap.createTextBitmap(String, String, int, int, int, int)} and still
 * receives the pixels through {@code nativeInitBitmapDC}) but:
 *
 * <ul>
 *   <li>measures with float precision and rounds up, so glyphs are never clipped;</li>
 *   <li>wraps on word boundaries with a correct character fallback for long words;</li>
 *   <li>honours explicit newlines, a maximum line count and an ellipsis for overflow;</li>
 *   <li>clamps the bitmap to the real maximum texture size, scaling the font down when a
 *       label is too large for the GPU instead of rendering nothing;</li>
 *   <li>always produces at least a 1x1 bitmap, so empty labels cannot throw.</li>
 * </ul>
 */
public final class ModernTextLayout {
    private static final String TAG = "ModernTextLayout";

    private static final int HALIGN_LEFT = 1;
    private static final int HALIGN_RIGHT = 2;
    private static final int HALIGN_CENTER = 3;
    private static final int VALIGN_TOP = 1;
    private static final int VALIGN_BOTTOM = 2;
    private static final int VALIGN_CENTER = 3;

    /** Conservative fallback when the GL context cannot be queried (old GLES2 drivers). */
    private static final int FALLBACK_MAX_TEXTURE = 2048;
    /** Absolutely never allocate a texture larger than this, whatever the driver reports. */
    private static final int HARD_MAX_TEXTURE = 8192;

    private static int sMaxTextureSize;
    private static Method sInitNativeObject;
    private static boolean sInitLookupFailed;

    private static Paint sCachedPaint;
    private static String sCachedFont;
    private static int sCachedSize = -1;
    private static int sCachedAlign = -1;

    private ModernTextLayout() {
    }

    /** Result of one layout pass (all values in pixels). */
    private static final class Layout {
        int width;
        int height;
        int lineHeight;
        final List<String> lines = new ArrayList<String>();
    }

    /**
     * Entry point replacing {@code Cocos2dxBitmap.createTextBitmap}. Called from native code
     * through the (patched) cocos2d-x Java class, so the signature and argument order must
     * stay identical to the 2013 build.
     *
     * @param text      string to render, may contain '\n'
     * @param fontName  typeface name ("Arial", or a bundled ".ttf")
     * @param fontSize  requested text size in pixels
     * @param alignment horizontal alignment in the low nibble, vertical in the high nibble
     * @param width     requested width, 0 = shrink to fit the text
     * @param height    requested height, 0 = grow to fit the text
     */
    public static void createTextBitmap(String text, String fontName, int fontSize,
                                        int alignment, int width, int height) {
        final Layout layout = createBitmap(text, fontName, fontSize, alignment, width, height);
        // The bitmap itself is produced inside createBitmapAndDraw; kept separate purely to
        // make the failure path (never throwing into native code) obvious.
    }

    private static Layout createBitmap(String text, String fontName, int fontSize,
                                       int alignment, int width, int height) {
        final int horizontalAlignment = Alignment.horizontal(alignment);
        final int verticalAlignment = Alignment.vertical(alignment);
        final int maxTexture = maxTextureSize();

        String content = normalize(text);
        int size = Math.max(1, Math.min(fontSize, maxTexture));
        int requestWidth = Math.max(0, width);
        int requestHeight = Math.max(0, height);

        Layout layout = null;
        Bitmap bitmap = null;

        // Shrink the font until the requested label fits inside one texture. Two passes are
        // normally enough; the loop is bounded so a pathological call can never spin.
        for (int attempt = 0; attempt < 6; attempt++) {
            final Paint paint = paint(fontName, size, horizontalAlignment);
            final Layout candidate = layoutText(content, paint, requestWidth, requestHeight, maxTexture);
            final int bitmapWidth = Math.max(1, requestWidth > 0 ? requestWidth : candidate.width);
            final int bitmapHeight = Math.max(1, requestHeight > 0 ? requestHeight : candidate.height);

            if (bitmapWidth <= maxTexture && bitmapHeight <= maxTexture) {
                layout = candidate;
                bitmap = Bitmap.createBitmap(bitmapWidth, bitmapHeight, Bitmap.Config.ARGB_8888);
                drawLayout(bitmap, candidate, paint, bitmapWidth, bitmapHeight, verticalAlignment);
                break;
            }

            // Too large for the GPU: scale the text down by the worst overflow factor and retry.
            final double scale = Math.min(
                    (double) maxTexture / (double) bitmapWidth,
                    (double) maxTexture / (double) bitmapHeight);
            final int next = (int) Math.floor(size * Math.max(0.05, scale));
            if (next >= size || next < 1) {
                // Cannot shrink any further (already 1px): cut the text and clamp the canvas.
                layout = candidate;
                final int w = Math.max(1, Math.min(bitmapWidth, maxTexture));
                final int h = Math.max(1, Math.min(bitmapHeight, maxTexture));
                bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                drawLayout(bitmap, candidate, paint, w, h, verticalAlignment);
                break;
            }
            size = next;
        }

        if (bitmap == null) {
            bitmap = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888);
        }
        if (layout == null) {
            layout = new Layout();
            layout.width = bitmap.getWidth();
            layout.height = bitmap.getHeight();
        }

        publish(bitmap);
        return layout;
    }

    private static void drawLayout(Bitmap bitmap, Layout layout, Paint paint,
                                   int bitmapWidth, int bitmapHeight, int verticalAlignment) {
        final Canvas canvas = new Canvas(bitmap);
        final Paint.FontMetricsInt fm = paint.getFontMetricsInt();
        final int textHeight = layout.lineHeight * layout.lines.size();
        int y;
        switch (verticalAlignment) {
            case VALIGN_BOTTOM:
                y = bitmapHeight - (textHeight - layout.lineHeight) - fm.bottom;
                break;
            case VALIGN_CENTER:
                y = (bitmapHeight - textHeight) / 2 - fm.top;
                break;
            case VALIGN_TOP:
            default:
                y = -fm.top;
                break;
        }
        for (int i = 0; i < layout.lines.size(); i++) {
            final String line = layout.lines.get(i);
            if (line.length() == 0) {
                y += layout.lineHeight;
                continue;
            }
            // Paint.TextAlign already places the origin for centre/right alignment.
            float x = 0f;
            if (paint.getTextAlign() == Paint.Align.CENTER) {
                x = bitmapWidth * 0.5f;
            } else if (paint.getTextAlign() == Paint.Align.RIGHT) {
                x = bitmapWidth;
            }
            canvas.drawText(line, x, y, paint);
            y += layout.lineHeight;
        }
    }

    private static Layout layoutText(String text, Paint paint, int maxWidth, int maxHeight,
                                     int maxTexture) {
        final Paint.FontMetricsInt fm = paint.getFontMetricsInt();
        final int lineHeight = Math.max(1, (int) Math.ceil(fm.bottom - fm.top));
        final List<String> source = new ArrayList<String>();
        int start = 0;
        for (int i = 0; i < text.length(); i++) {
            if (text.charAt(i) == '\n') {
                source.add(text.substring(start, i));
                start = i + 1;
            }
        }
        source.add(text.substring(start));

        final List<String> lines = new ArrayList<String>();
        for (String paragraph : source) {
            if (maxWidth <= 0) {
                lines.add(paragraph);
                continue;
            }
            wrap(paragraph, paint, maxWidth, lines);
        }

        int maxLines = 0;
        if (maxHeight > 0) {
            maxLines = Math.max(1, maxHeight / lineHeight);
            if (lines.size() > maxLines) {
                final List<String> trimmed = new ArrayList<String>(maxLines);
                for (int i = 0; i < maxLines; i++) {
                    trimmed.add(lines.get(i));
                }
                trimmed.set(maxLines - 1, ellipsize(trimmed.get(maxLines - 1), paint, maxWidth));
                return buildLayout(trimmed, paint, lineHeight, maxWidth, maxTexture);
            }
        }

        final Layout layout = buildLayout(lines, paint, lineHeight, maxWidth, maxTexture);
        if (maxHeight > 0) {
            layout.height = Math.min(Math.max(maxHeight, lineHeight), maxTexture);
        }
        return layout;
    }

    private static Layout buildLayout(List<String> lines, Paint paint, int lineHeight,
                                      int maxWidth, int maxTexture) {
        final Layout layout = new Layout();
        layout.lines.addAll(lines);
        layout.lineHeight = lineHeight;
        int width = 0;
        for (String line : lines) {
            width = Math.max(width, (int) Math.ceil(paint.measureText(line)));
        }
        if (maxWidth > 0) {
            width = Math.min(maxWidth, maxTexture);
        }
        layout.width = Math.max(1, Math.min(width + 1, maxTexture)); // +1 px: antialiased edge
        layout.height = Math.max(1, Math.min(lineHeight * Math.max(1, lines.size()), maxTexture));
        return layout;
    }

    /** Greedy word wrap with a character fallback, correct for wide glyphs and CJK text. */
    private static void wrap(String paragraph, Paint paint, int maxWidth, List<String> out) {
        if (paragraph.length() == 0) {
            out.add("");
            return;
        }
        int lineStart = 0;
        int lastBreak = -1;
        for (int i = 1; i <= paragraph.length(); i++) {
            if (i < paragraph.length() && paragraph.charAt(i - 1) == ' ') {
                lastBreak = i - 1;
            }
            final float w = paint.measureText(paragraph, lineStart, i);
            if (w <= maxWidth || i == lineStart + 1) {
                continue;
            }
            int cut = (lastBreak > lineStart) ? lastBreak : i - 1;
            if (cut <= lineStart) {
                cut = i - 1;
            }
            out.add(paragraph.substring(lineStart, cut));
            lineStart = cut;
            // Skip the space(s) that produced the break.
            while (lineStart < paragraph.length() && paragraph.charAt(lineStart) == ' ') {
                lineStart++;
            }
            lastBreak = -1;
            i = lineStart;
        }
        out.add(paragraph.substring(Math.min(lineStart, paragraph.length())));
    }

    private static String ellipsize(String line, Paint paint, int maxWidth) {
        final String suffix = "...";
        if (paint.measureText(line) <= maxWidth) {
            return line;
        }
        int end = line.length();
        while (end > 0 && paint.measureText(line, 0, end) + paint.measureText(suffix) > maxWidth) {
            end--;
        }
        return line.substring(0, end) + suffix;
    }

    private static String normalize(String text) {
        if (text == null) {
            return " ";
        }
        String s = text.replace("\r\n", "\n").replace('\r', '\n');
        // The 2013 engine passes "" for an empty CCLabelTTF; keep that path non-crashing.
        if (s.length() == 0) {
            return " ";
        }
        return s;
    }

    private static Paint paint(String fontName, int fontSize, int horizontalAlignment) {
        if (sCachedPaint != null && fontSize == sCachedSize
                && horizontalAlignment == sCachedAlign
                && (fontName == null ? sCachedFont == null : fontName.equals(sCachedFont))) {
            return sCachedPaint;
        }
        final Paint paint = new Paint();
        paint.setColor(Color.WHITE);
        paint.setTextSize(fontSize);
        paint.setAntiAlias(true);
        paint.setSubpixelText(true);
        paint.setFilterBitmap(true);

        Typeface typeface;
        try {
            typeface = Typeface.create(fontName == null ? "Arial" : fontName, Typeface.NORMAL);
        } catch (Throwable t) {
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.NORMAL);
        }
        paint.setTypeface(typeface == null ? Typeface.create(Typeface.DEFAULT, Typeface.NORMAL) : typeface);

        switch (horizontalAlignment) {
            case HALIGN_CENTER:
                paint.setTextAlign(Paint.Align.CENTER);
                break;
            case HALIGN_RIGHT:
                paint.setTextAlign(Paint.Align.RIGHT);
                break;
            case HALIGN_LEFT:
            default:
                paint.setTextAlign(Paint.Align.LEFT);
                break;
        }

        sCachedPaint = paint;
        sCachedFont = fontName;
        sCachedSize = fontSize;
        sCachedAlign = horizontalAlignment;
        return paint;
    }

    /**
     * Queries the real maximum texture size from the GL thread. The native render thread is
     * the one calling in here, so a current context is always available; the call is still
     * wrapped because text can also be created during the loading screen on some drivers.
     */
    private static int maxTextureSize() {
        int cached = sMaxTextureSize;
        if (cached > 0) {
            return cached;
        }
        int size = FALLBACK_MAX_TEXTURE;
        try {
            final int[] out = new int[1];
            GLES20.glGetIntegerv(GLES20.GL_MAX_TEXTURE_SIZE, out, 0);
            if (out[0] >= 64) {
                size = Math.min(out[0], HARD_MAX_TEXTURE);
            }
        } catch (Throwable t) {
            Log.w(TAG, "GL_MAX_TEXTURE_SIZE unavailable, assuming " + FALLBACK_MAX_TEXTURE, t);
        }
        sMaxTextureSize = size;
        return size;
    }

    /** Hands the finished bitmap to the engine through cocos2d-x's own native callback. */
    private static void publish(Bitmap bitmap) {
        try {
            Method m = sInitNativeObject;
            if (m == null && !sInitLookupFailed) {
                final Class<?> cb = Class.forName("org.cocos2dx.lib.Cocos2dxBitmap");
                m = cb.getMethod("initNativeObject", Bitmap.class);
                sInitNativeObject = m;
            }
            if (m != null) {
                m.invoke(null, bitmap);
                return;
            }
        } catch (Throwable t) {
            Log.e(TAG, "cocos2d-x text callback unavailable", t);
        }
        // Last resort: upload directly through the private native callback.
        try {
            final Class<?> cb = Class.forName("org.cocos2dx.lib.Cocos2dxBitmap");
            final Method nativeInit = cb.getDeclaredMethod("nativeInitBitmapDC", int.class, int.class, byte[].class);
            nativeInit.setAccessible(true);
            final int w = bitmap.getWidth();
            final int h = bitmap.getHeight();
            final int[] argb = new int[w * h];
            bitmap.getPixels(argb, 0, w, 0, 0, w, h);
            final byte[] rgba = new byte[w * h * 4];
            for (int i = 0; i < argb.length; i++) {
                final int px = argb[i];
                rgba[i * 4] = (byte) ((px >> 16) & 0xFF);
                rgba[i * 4 + 1] = (byte) ((px >> 8) & 0xFF);
                rgba[i * 4 + 2] = (byte) (px & 0xFF);
                rgba[i * 4 + 3] = (byte) ((px >>> 24) & 0xFF);
            }
            nativeInit.invoke(null, w, h, rgba);
        } catch (Throwable t) {
            Log.e(TAG, "native text upload failed", t);
        }
    }

    private static final class Alignment {
        static int horizontal(int alignment) {
            final int h = alignment & 0x0F;
            return (h == HALIGN_CENTER || h == HALIGN_RIGHT || h == HALIGN_LEFT) ? h : HALIGN_LEFT;
        }

        static int vertical(int alignment) {
            final int v = (alignment >> 4) & 0x0F;
            return (v == VALIGN_CENTER || v == VALIGN_BOTTOM || v == VALIGN_TOP) ? v : VALIGN_TOP;
        }
    }
}
