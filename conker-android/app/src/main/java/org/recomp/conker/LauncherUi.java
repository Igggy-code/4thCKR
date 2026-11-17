package org.recomp.conker;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.TextView;

/** Look of the launcher screens: warm cartoon-pub colours, PT Serif Bold
 *  Italic for titles (close to the game's menu titles) and PT Sans Caption
 *  Bold for text (close to the game's speech font). */
final class LauncherUi {
    static final int CREAM = 0xFFFFF1D6, AMBER = 0xFFF2A23A, ORANGE = 0xFFD9661F,
            BROWN = 0xFF2A1608, DARK = 0xFF120904, MUTED = 0xFFD9C4A3;

    private static Typeface title, body;

    static Typeface titleFace(Context c) {
        if (title == null) title = load(c, "fonts/PTSerif-BoldItalic.ttf", Typeface.create(Typeface.SERIF, Typeface.BOLD_ITALIC));
        return title;
    }
    static Typeface bodyFace(Context c) {
        if (body == null) body = load(c, "fonts/PTSansCaption-Bold.ttf", Typeface.DEFAULT_BOLD);
        return body;
    }
    private static Typeface load(Context c, String asset, Typeface fallback) {
        try { return Typeface.createFromAsset(c.getAssets(), asset); } catch (Exception e) { return fallback; }
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics());
    }

    static TextView text(Context c, String s, float sp, int color, boolean titleFont) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        t.setTypeface(titleFont ? titleFace(c) : bodyFace(c));
        t.setGravity(Gravity.CENTER);
        t.setShadowLayer(dp(c, 3), 0, dp(c, 2), 0xC0000000);
        return t;
    }

    /** Pill button, amber when idle, darker when pressed, grey when disabled. */
    static TextView button(Context c, String s) {
        TextView b = text(c, s, 18, BROWN, false);
        b.setShadowLayer(0, 0, 0, 0);
        int r = dp(c, 28);
        GradientDrawable up = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] { 0xFFFFC266, AMBER, ORANGE });
        up.setCornerRadius(r); up.setStroke(dp(c, 2), 0xFF5A2E0E);
        GradientDrawable down = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, new int[] { ORANGE, 0xFFB04E12 });
        down.setCornerRadius(r); down.setStroke(dp(c, 2), 0xFF5A2E0E);
        GradientDrawable off = new GradientDrawable(); off.setColor(0xFF5B4A3A); off.setCornerRadius(r);
        StateListDrawable sl = new StateListDrawable();
        sl.addState(new int[] { -android.R.attr.state_enabled }, off);
        sl.addState(new int[] { android.R.attr.state_pressed }, down);
        sl.addState(new int[] {}, up);
        b.setBackground(sl);
        b.setPadding(dp(c, 36), dp(c, 14), dp(c, 36), dp(c, 14));
        b.setClickable(true);
        b.setFocusable(true);
        return b;
    }

    /** Dark warm background with a soft light in the upper middle. */
    static final class Backdrop extends View {
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        Backdrop(Context c) { super(c); }
        @Override protected void onDraw(Canvas cv) {
            int w = getWidth(), h = getHeight();
            p.setShader(new LinearGradient(0, 0, 0, h, new int[] { 0xFF3B1E0B, 0xFF1E0F06, DARK }, null, Shader.TileMode.CLAMP));
            cv.drawRect(0, 0, w, h, p);
            p.setShader(new RadialGradient(w * 0.5f, h * 0.28f, Math.max(w, h) * 0.55f,
                    new int[] { 0x55F2A23A, 0x00F2A23A }, null, Shader.TileMode.CLAMP));
            cv.drawRect(0, 0, w, h, p);
            p.setShader(null);
        }
    }

    /** Rounded progress bar with moving stripes; determinate (0..1) or, with
     *  a negative value, a sliding band (waiting with no known end). */
    static final class Bar extends View {
        private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG),
                stripe = new Paint(Paint.ANTI_ALIAS_FLAG), edge = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float value = -1f;
        private final long t0 = System.currentTimeMillis();
        Bar(Context c) {
            super(c);
            track.setColor(0xFF1A0D05);
            edge.setStyle(Paint.Style.STROKE); edge.setStrokeWidth(dp(c, 2)); edge.setColor(0xFF6B3A16);
            stripe.setColor(0x33FFFFFF);
        }
        void set(float v) { value = v; invalidate(); }
        /** The in-game look: the title's own dark-teal backdrop and cold blue
         *  glow, so it hands over smoothly to the game's loading screens. */
        private boolean blue;
        void setBlue() {
            blue = true;
            track.setColor(0xD0051019);
            edge.setColor(0xFF2B7FA6);
            edge.setStrokeWidth(dp(getContext(), 1.5f));
            stripe.setColor(0x22FFFFFF);
        }
        @Override protected void onMeasure(int ws, int hs) {
            setMeasuredDimension(MeasureSpec.getSize(ws), dp(getContext(), 22));
        }
        @Override public void draw(Canvas cv) { onDraw(cv); }
        { setWillNotDraw(false);
        }
        @Override protected void onDraw(Canvas cv) {
            float w = getWidth(), h = getHeight(), r = h / 2f, in = dp(getContext(), 3);
            RectF all = new RectF(1, 1, w - 1, h - 1);
            cv.drawRoundRect(all, r, r, track);
            float t = (System.currentTimeMillis() - t0) / 1000f;
            float x0 = in, x1;
            if (value >= 0) x1 = in + (w - 2 * in) * Math.min(1f, value);
            else { float band = (w - 2 * in) * 0.3f, pos = (t * 0.6f) % 1.3f - 0.3f;
                   x0 = in + (w - 2 * in) * Math.max(0, pos); x1 = Math.min(w - in, x0 + band); }
            if (x1 > x0 + 1) {
                RectF f = new RectF(x0, in, x1, h - in);
                fill.setShader(new LinearGradient(0, in, 0, h - in, blue
                        ? new int[] { 0xFFC8F4FF, 0xFF54C8F2, 0xFF1C6FB4 }
                        : new int[] { 0xFFFFD27A, AMBER, ORANGE }, null, Shader.TileMode.CLAMP));
                cv.drawRoundRect(f, r - in, r - in, fill);
                cv.save();
                Path clip = new Path(); clip.addRoundRect(f, r - in, r - in, Path.Direction.CW);
                cv.clipPath(clip);
                float step = h * 1.2f, off = (t * h * 1.6f) % step;
                for (float sx = x0 - h + off - step; sx < x1 + h; sx += step) {
                    Path s = new Path();
                    s.moveTo(sx, h); s.lineTo(sx + step / 2f, h); s.lineTo(sx + step / 2f + h, 0); s.lineTo(sx + h, 0); s.close();
                    cv.drawPath(s, stripe);
                }
                cv.restore();
            }
            cv.drawRoundRect(all, r, r, edge);
            postInvalidateOnAnimation();
        }
    }

    static int color(int c, int alpha) { return Color.argb(alpha, Color.red(c), Color.green(c), Color.blue(c)); }
}
