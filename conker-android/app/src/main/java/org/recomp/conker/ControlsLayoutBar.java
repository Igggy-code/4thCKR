package org.recomp.conker;

import android.app.Activity;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.SeekBar;
import android.widget.TextView;

/** Settings -> Configure controls layout: the bar over the on-screen controller
 *  while the player moves and resizes its buttons, per mode (basic / aiming).
 *  The buttons themselves are dragged on TouchGamepadView (its edit mode). */
final class ControlsLayoutBar extends FrameLayout {
    private static final int CREAM = 0xFFF4E6C8, CREAM_DIM = 0xB3F4E6C8, AMBER = 0xFFF2B35A,
            CARD = 0xEB1E1812, LINE = 0x33FFFFFF, TONAL = 0x29F2B35A, INK = 0xFF2A1C0C;
    private static final String[] MODES = { "Basic controls", "Aiming (shooting)" };
    private static ControlsLayoutBar sOpen;

    private final TouchGamepadView pad;
    private final float dp;
    private final TextView mode, selName, pct, hint;
    private final SeekBar scale;
    private int layer = TouchGamepadView.LAYER_BASIC;
    private boolean quiet;   // setting the slider from code

    static void open(Activity a, ViewGroup root, TouchGamepadView pad) {
        if (sOpen != null) return;
        ControlsLayoutBar b = new ControlsLayoutBar(a, pad);
        root.addView(b, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        sOpen = b;
        pad.beginLayoutEdit(TouchGamepadView.LAYER_BASIC, b::onSelection);
    }

    /** Back while editing: done. */
    static boolean closeIfOpen() {
        if (sOpen == null) return false;
        sOpen.finish();
        return true;
    }

    private ControlsLayoutBar(Activity a, TouchGamepadView p) {
        super(a);
        pad = p;
        dp = a.getResources().getDisplayMetrics().density;
        // the full-screen frame takes no touches: they reach the controller below

        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(round(CARD, 22, LINE));
        card.setElevation(10 * dp);
        card.setPadding(px(14), px(8), px(10), px(8));
        card.setClickable(true);   // touches on the bar stay on the bar

        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        card.addView(row);

        // mode: a pill with a chevron, a rounded menu under it
        mode = new TextView(a);
        mode.setTextColor(CREAM);
        mode.setTextSize(14);
        mode.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        mode.setGravity(Gravity.CENTER_VERTICAL);
        mode.setPadding(px(14), 0, px(12), 0);
        mode.setBackground(round(TONAL, 18, 0x66F2B35A));
        mode.setOnClickListener(v -> showModes());
        row.addView(mode, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, px(36)));

        selName = label("", 13, CREAM);
        LinearLayout.LayoutParams nl = new LinearLayout.LayoutParams(px(118), ViewGroup.LayoutParams.WRAP_CONTENT);
        nl.setMargins(px(12), 0, 0, 0);
        row.addView(selName, nl);

        scale = new SeekBar(a);
        scale.setMax(250);   // 50 .. 300 %
        scale.setProgressTintList(android.content.res.ColorStateList.valueOf(AMBER));
        scale.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(0x55FFFFFF));
        scale.setThumbTintList(android.content.res.ColorStateList.valueOf(AMBER));
        row.addView(scale, new LinearLayout.LayoutParams(px(170), ViewGroup.LayoutParams.WRAP_CONTENT));
        pct = label("100%", 13, CREAM);
        row.addView(pct, new LinearLayout.LayoutParams(px(46), ViewGroup.LayoutParams.WRAP_CONTENT));
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int v, boolean user) {
                pct.setText((v + 50) + "%");
                if (!quiet) pad.setEditScale(v + 50);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) { }
            @Override public void onStopTrackingTouch(SeekBar sb) { }
        });

        row.addView(pill("Reset to default", false, v -> pad.resetEditLayer()));
        row.addView(pill("Done", true, v -> finish()));

        hint = label("", 11.5f, CREAM_DIM);
        hint.setGravity(Gravity.START);
        LinearLayout.LayoutParams hl = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        hl.setMargins(px(4), px(5), 0, 0);
        card.addView(hint, hl);

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        lp.topMargin = px(84);   // below the L / R / START row
        addView(card, lp);
        setLayer(TouchGamepadView.LAYER_BASIC);
        onSelection(null, 100);
    }

    private void setLayer(int l) {
        layer = l;
        mode.setText(MODES[l] + "   ▾");
        pad.setEditLayer(l);
    }

    private void showModes() {
        LinearLayout list = new LinearLayout(getContext());
        list.setOrientation(LinearLayout.VERTICAL);
        list.setBackground(round(0xF5241C14, 16, LINE));
        list.setPadding(0, px(6), 0, px(6));
        PopupWindow pw = new PopupWindow(list, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, true);
        for (int i = 0; i < MODES.length; i++) {
            final int l = i;
            boolean on = i == layer;
            TextView t = label((on ? "✓   " : "     ") + MODES[i], 14, on ? AMBER : CREAM);
            t.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            t.setPadding(px(16), 0, px(22), 0);
            t.setMinWidth(px(220));
            t.setOnClickListener(v -> { pw.dismiss(); if (l != layer) setLayer(l); });
            list.addView(t, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, px(44)));
        }
        pw.setBackgroundDrawable(new ColorDrawable(0));   // the list draws its own card
        pw.setOutsideTouchable(true);
        pw.setElevation(12 * dp);
        pw.showAsDropDown(mode, 0, px(6));
    }

    private void onSelection(String name, int scalePct) {
        boolean sel = name != null;
        selName.setText(sel ? "Button " + name : "No button selected");
        hint.setText(sel ? "Drag it to move it · use the slider to resize it · tap another button to switch"
                         : "Tap a button to select it, then drag it where you want it");
        scale.setEnabled(sel);
        scale.setAlpha(sel ? 1f : 0.35f);
        pct.setAlpha(sel ? 1f : 0.35f);
        quiet = true;
        scale.setProgress(Math.max(0, Math.min(250, scalePct - 50)));
        quiet = false;
        pct.setText(scalePct + "%");
    }

    private void finish() {
        pad.endLayoutEdit();
        ViewGroup parent = (ViewGroup) getParent();
        if (parent != null) parent.removeView(this);
        if (sOpen == this) sOpen = null;
    }

    private int px(float v) { return (int) (v * dp + 0.5f); }

    private GradientDrawable round(int color, float radiusDp, int strokeColor) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radiusDp * dp);
        if (strokeColor != 0) g.setStroke(Math.max(1, px(1)), strokeColor);
        return g;
    }

    private TextView label(String s, float size, int color) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextColor(color);
        t.setTextSize(size);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    /** rounded pill button: filled (primary) or tonal */
    private TextView pill(String s, boolean primary, View.OnClickListener l) {
        TextView b = label(s, 13.5f, primary ? INK : CREAM);
        b.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        b.setPadding(px(16), 0, px(16), 0);
        b.setBackground(round(primary ? AMBER : TONAL, 18, primary ? 0 : 0x66F2B35A));
        b.setClickable(true);
        b.setOnClickListener(l);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, px(36));
        lp.setMargins(px(10), 0, 0, 0);
        b.setLayoutParams(lp);
        return b;
    }
}
