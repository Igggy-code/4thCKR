package org.recomp.conker;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

/** Settings -> Configure controls layout: the bar over the on-screen controller
 *  while the player moves and resizes its buttons, per mode (basic / aiming).
 *  The buttons themselves are dragged on TouchGamepadView (its edit mode). */
final class ControlsLayoutBar extends FrameLayout {
    private static final int CREAM = 0xFFF4E6C8, AMBER = 0xFFE09A3E;
    private static ControlsLayoutBar sOpen;

    private final TouchGamepadView pad;
    private final boolean ru;
    private final float dp;
    private final TextView selName, pct;
    private final SeekBar scale;
    private boolean quiet;   // setting the slider from code

    static void open(Activity a, ViewGroup root, TouchGamepadView pad, boolean ru) {
        if (sOpen != null) return;
        ControlsLayoutBar b = new ControlsLayoutBar(a, pad, ru);
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

    private ControlsLayoutBar(Activity a, TouchGamepadView p, boolean ru) {
        super(a);
        pad = p; this.ru = ru;
        dp = a.getResources().getDisplayMetrics().density;
        // the full-screen frame takes no touches: they reach the controller below

        LinearLayout bar = new LinearLayout(a);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { 0xF0211208, 0xF03A2410 });
        bg.setCornerRadius(12 * dp);
        bg.setStroke((int) Math.max(1, dp), 0xFFB07A32);
        bar.setBackground(bg);
        int pd = (int) (10 * dp);
        bar.setPadding(pd, (int) (4 * dp), pd, (int) (4 * dp));
        bar.setClickable(true);   // touches on the bar stay on the bar

        Spinner mode = new Spinner(a, Spinner.MODE_DROPDOWN);
        String[] modes = ru ? new String[] { "Базовое управление", "Прицеливание (стрельба)" }
                            : new String[] { "Basic controls", "Aiming (shooting)" };
        ArrayAdapter<String> ad = new ArrayAdapter<String>(a, android.R.layout.simple_spinner_item, modes) {
            @Override public View getView(int pos, View cv, ViewGroup parent) {
                TextView t = (TextView) super.getView(pos, cv, parent);
                t.setTextColor(CREAM); t.setTextSize(14);
                return t;
            }
        };
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        mode.setAdapter(ad);
        mode.setPopupBackgroundDrawable(new android.graphics.drawable.ColorDrawable(0xFFF4E6C8));
        mode.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View v, int pos, long id) {
                pad.setEditLayer(pos == 1 ? TouchGamepadView.LAYER_AIM : TouchGamepadView.LAYER_BASIC);
            }
            @Override public void onNothingSelected(AdapterView<?> parent) { }
        });
        bar.addView(mode, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, (int) (40 * dp)));

        selName = label("");
        LinearLayout.LayoutParams nl = new LinearLayout.LayoutParams((int) (110 * dp), ViewGroup.LayoutParams.WRAP_CONTENT);
        nl.setMargins((int) (8 * dp), 0, 0, 0);
        bar.addView(selName, nl);

        scale = new SeekBar(a);
        scale.setMax(250);   // 50 .. 300 %
        scale.setProgressTintList(android.content.res.ColorStateList.valueOf(AMBER));
        scale.setThumbTintList(android.content.res.ColorStateList.valueOf(AMBER));
        scale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int v, boolean user) {
                pct.setText((v + 50) + "%");
                if (!quiet) pad.setEditScale(v + 50);
            }
            @Override public void onStartTrackingTouch(SeekBar sb) { }
            @Override public void onStopTrackingTouch(SeekBar sb) { }
        });
        bar.addView(scale, new LinearLayout.LayoutParams((int) (170 * dp), ViewGroup.LayoutParams.WRAP_CONTENT));
        pct = label("100%");
        bar.addView(pct, new LinearLayout.LayoutParams((int) (48 * dp), ViewGroup.LayoutParams.WRAP_CONTENT));

        bar.addView(button(ru ? "Вернуть по умолчанию" : "Reset to default", v -> pad.resetEditLayer()));
        bar.addView(button(ru ? "Готово" : "Done", v -> finish()));

        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        lp.topMargin = (int) (84 * dp);   // below the L / R / START row
        addView(bar, lp);
        onSelection(null, 100);
    }

    private void onSelection(String name, int scalePct) {
        boolean sel = name != null;
        selName.setText(sel ? (ru ? "Кнопка: " : "Button: ") + name : (ru ? "Выберите кнопку" : "Pick a button"));
        scale.setEnabled(sel);
        scale.setAlpha(sel ? 1f : 0.4f);
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

    private TextView label(String s) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextColor(CREAM);
        t.setTextSize(13);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(getContext());
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(CREAM);
        b.setTextSize(13);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF5A3A1E);
        bg.setStroke((int) Math.max(1, dp), 0xFFB07A32);
        bg.setCornerRadius(8 * dp);
        b.setBackground(bg);
        b.setPadding((int) (12 * dp), 0, (int) (12 * dp), 0);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, (int) (38 * dp));
        lp.setMargins((int) (8 * dp), 0, 0, 0);
        b.setLayoutParams(lp);
        b.setOnClickListener(l);
        return b;
    }
}
