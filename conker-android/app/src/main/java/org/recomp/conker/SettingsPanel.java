package org.recomp.conker;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

/** Side menu: opened with Back or a swipe from a screen edge. */
final class SettingsPanel extends FrameLayout {
    static final int REQ_DRIVER = 4711;
    // the bar's palette: dark wood, candle amber, cream
    private static final int GOLD = 0xFFF2C46B, AMBER = 0xFFE09A3E, CREAM = 0xFFF4E6C8, CREAM_DIM = 0xFFC9B48E;

    private android.graphics.drawable.Drawable sheetBg() {
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] { 0xF5211208, 0xF53A2410 });
        return g;
    }

    private final Activity act;
    private final Settings st;
    private final TouchGamepadView pad;
    private final float dp;
    private final View scrim;
    private final ScrollView sheet;
    private final LinearLayout body;
    private boolean open;
    private boolean restartNeeded;
    private TextView restartNote;
    private Button restartBtn;
    // hidden REC button: 8 quick taps on the title show it, 3 hide it
    private int titleTaps;
    private long lastTitleTap;

    SettingsPanel(Activity a, Settings s, TouchGamepadView p) {
        super(a);
        act = a; st = s; pad = p;
        dp = a.getResources().getDisplayMetrics().density;
        setVisibility(GONE);

        scrim = new View(a);
        scrim.setBackgroundColor(0x88000000);
        scrim.setOnClickListener(v -> close());
        addView(scrim, new LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT));

        sheet = new ScrollView(a);
        sheet.setBackground(sheetBg());
        sheet.setClickable(true);
        body = new LinearLayout(a);
        body.setOrientation(LinearLayout.VERTICAL);
        int pd = (int) (20 * dp);
        body.setPadding(pd, (int) (16 * dp), pd, pd);
        sheet.addView(body, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        LayoutParams lp = new LayoutParams((int) (380 * dp), LayoutParams.MATCH_PARENT, Gravity.END);
        addView(sheet, lp);
        build();
    }

    boolean isOpen() { return open; }

    void toggle() { if (open) close(); else show(); }

    void show() {
        if (open) return;
        open = true;
        build();
        setVisibility(VISIBLE);
        int w = Math.min((int) (380 * dp), (int) (getWidth() > 0 ? getWidth() * 0.6f : 380 * dp));
        sheet.getLayoutParams().width = w;
        sheet.requestLayout();
        sheet.setTranslationX(w);
        scrim.setAlpha(0f);
        sheet.animate().translationX(0).setDuration(180).start();
        scrim.animate().alpha(1f).setDuration(180).start();
        pad.setMenuOpen(true);
    }

    void close() {
        if (!open) return;
        open = false;
        float w = sheet.getWidth();
        sheet.animate().translationX(w).setDuration(160).start();
        scrim.animate().alpha(0f).setDuration(160).withEndAction(() -> { if (!open) setVisibility(GONE); }).start();
        pad.setMenuOpen(false);
    }

    // ---- content ---------------------------------------------------------------------------

    private void build() {
        body.removeAllViews();

        TextView title = text("Settings", 22, true);
        title.setTextColor(GOLD);
        title.setTypeface(Typeface.create(Typeface.SERIF, Typeface.BOLD_ITALIC));
        title.setPadding(0, 0, 0, (int) (6 * dp));
        title.setOnClickListener(v -> titleTap());
        body.addView(title);

        // on-screen controller
        section("On-screen controller");
        RadioGroup g = group();
        addRadio(g, Settings.PAD_AUTO, "Auto (cinema mode)");
        addRadio(g, Settings.PAD_ALWAYS, "Always shown");
        addRadio(g, Settings.PAD_HIDDEN, "Off (hidden)");
        g.check(st.padMode());
        g.setOnCheckedChangeListener((gr, id) -> { st.setPadMode(id); pad.setPadMode(id); });
        body.addView(g);

        // render resolution
        section("Render resolution");
        RadioGroup r = group();
        addRadio(r, 1, "x1 — 640×480");
        addRadio(r, 2, "x2 — 1280×960 (sharper, slower)");
        body.addView(note("Takes effect after a restart."));
        r.check(st.scale());
        r.setOnCheckedChangeListener((gr, id) -> { st.setScale(id); needRestart(); });
        body.addView(r);

        // sharpness
        section("Sharpness");
        RadioGroup sh = group();
        addRadio(sh, 0, "Off (smooth)");
        addRadio(sh, 1, "Low");
        addRadio(sh, 2, "Medium");
        addRadio(sh, 3, "High (original)");
        sh.check(st.sharpness());
        sh.setOnCheckedChangeListener((gr, id) -> { st.setSharpness(id); applyDisplayFx(); });
        body.addView(sh);

        // anti-aliasing
        section("Anti-aliasing");
        RadioGroup aa = group();
        addRadio(aa, 1, "On (FXAA)");
        addRadio(aa, 0, "Off");
        aa.check(st.aa() ? 1 : 0);
        aa.setOnCheckedChangeListener((gr, id) -> { st.setAa(id == 1); applyDisplayFx(); });
        body.addView(aa);

        // frame rate: developer mode only (shown with the REC button). Above 30
        // the game's own logic runs too fast in places (not fixed yet).
        if (st.recVisible()) {
        section("Frame rate (developer)");
        RadioGroup fr = group();
        addRadio(fr, 30, "30 FPS (original)");
        addRadio(fr, 40, "40 FPS (experimental; best on 120 Hz screens)");
        addRadio(fr, 60, "60 FPS (experimental; needs a fast phone)");
        body.addView(note("Developer option. Takes effect at once. Above 30 parts of the game run too fast."));
        fr.check(st.savedFps());
        fr.setOnCheckedChangeListener((gr, id) -> {
            st.setFps(id);
            try { TouchGamepadView.nativeSetFps(id); } catch (UnsatisfiedLinkError ignored) { }
        });
        body.addView(fr);
        }

        // aspect
        section("Aspect ratio");
        RadioGroup a = group();
        addRadio(a, 0, "Widescreen");
        addRadio(a, 1, "4:3 (original)");
        body.addView(note("Takes effect after a restart."));
        a.check(st.aspect43() ? 1 : 0);
        a.setOnCheckedChangeListener((gr, id) -> { st.setAspect43(id == 1); needRestart(); });
        body.addView(a);

        // text language
        section("Text language");
        RadioGroup lg = group();
        addRadio(lg, 0, "English");
        addRadio(lg, 1, "Русский");
        body.addView(note("Takes effect after a restart."));
        lg.check("ru".equals(st.language()) ? 1 : 0);
        lg.setOnCheckedChangeListener((gr, id) -> { st.setLanguage(id == 1 ? "ru" : "en"); needRestart(); });
        body.addView(lg);

        // GPU driver
        section("GPU driver");
        boolean active = false;
        try { active = TouchGamepadView.nativeDriverActive() != 0; } catch (Throwable ignored) { }
        String ds = st.lastDriverStatus();
        body.addView(note("In use: " + (active ? "custom driver" : "system driver")
                + (ds.isEmpty() ? "" : " (" + ds + ")")
                + ". A change takes effect after a restart."));
        RadioGroup d = group();
        List<String> custom = st.customDrivers();
        String[] ids = new String[2 + custom.size()];
        ids[0] = Settings.DRIVER_AUTO; ids[1] = Settings.DRIVER_SYSTEM;
        addRadio(d, 0, "Auto");
        addRadio(d, 1, "System");
        for (int i = 0; i < custom.size(); i++) {
            ids[2 + i] = Settings.CUSTOM_PREFIX + custom.get(i);
            addRadio(d, 2 + i, custom.get(i));
        }
        int sel = 0;
        for (int i = 0; i < ids.length; i++) if (ids[i].equals(st.driver())) sel = i;
        d.check(sel);
        d.setOnCheckedChangeListener((gr, id) -> { st.setDriver(ids[id]); needRestart(); });
        body.addView(d);
        LinearLayout row = new LinearLayout(act);
        row.setOrientation(LinearLayout.HORIZONTAL);
        Button inst = button("Install driver (.zip)…", v -> pickDriver());
        row.addView(inst);
        if (st.driver().startsWith(Settings.CUSTOM_PREFIX)) {
            Button del = button("Remove", v -> { st.removeCustom(st.driver()); needRestart(); build(); });
            row.addView(del);
        }
        body.addView(row);

        // restart / back
        restartNote = note("Changes apply after a restart.");
        restartNote.setTextColor(GOLD);
        restartNote.setVisibility(restartNeeded ? VISIBLE : GONE);
        body.addView(restartNote);
        LinearLayout bottom = new LinearLayout(act);
        bottom.setOrientation(LinearLayout.HORIZONTAL);
        bottom.setPadding(0, (int) (14 * dp), 0, 0);
        restartBtn = button("Restart", v -> restart());
        restartBtn.setVisibility(restartNeeded ? VISIBLE : GONE);
        bottom.addView(button("Resume", v -> close()));
        bottom.addView(restartBtn);
        body.addView(bottom);
        if (st.recVisible()) body.addView(note("REC button on (tap the title 3 times to hide)."));

        // logs for a bug report: one zip in Download
        Button logs = button("Save logs for a bug report", v -> saveLogs());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, (int) (14 * dp), 0, 0);
        body.addView(logs, lp);

        // disc image: a one-off, so one small line at the very bottom
        LinearLayout disc = new LinearLayout(act);
        disc.setOrientation(LinearLayout.HORIZONTAL);
        disc.setGravity(android.view.Gravity.CENTER_VERTICAL);
        disc.setPadding(0, (int) (22 * dp), 0, 0);
        TextView dl = note("Disc image: " + ("ru".equals(st.imageKind()) ? "Russian (fan)" : "English")
                + " · saves are kept");
        disc.addView(dl, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button img = button("Change ISO…", v -> {
            android.content.Intent i = new android.content.Intent(getContext(), SetupActivity.class);
            i.putExtra(SetupActivity.EXTRA_FORCE, true);
            getContext().startActivity(i);
            ((android.app.Activity) getContext()).finish();
            android.os.Process.killProcess(android.os.Process.myPid());
        });
        img.setTextSize(12);
        LinearLayout.LayoutParams ip = (LinearLayout.LayoutParams) img.getLayoutParams();
        ip.height = (int) (32 * dp);
        ip.setMargins(0, 0, 0, 0);
        disc.addView(img, ip);
        body.addView(disc);
    }

    /** Sharpness and anti-aliasing apply at once (no restart). */
    private void applyDisplayFx() {
        try { TouchGamepadView.nativeSetDisplayFx(st.sharpness(), st.aa() ? 1 : 0); }
        catch (Throwable t) { needRestart(); }
    }

    private void needRestart() {
        Toast.makeText(act, "Takes effect after a restart (Restart button below)", Toast.LENGTH_LONG).show();
        restartNeeded = true;
        if (restartNote != null) restartNote.setVisibility(VISIBLE);
        if (restartBtn != null) restartBtn.setVisibility(VISIBLE);
    }

    private void titleTap() {
        long now = System.currentTimeMillis();
        titleTaps = now - lastTitleTap < 600 ? titleTaps + 1 : 1;
        lastTitleTap = now;
        boolean vis = st.recVisible();
        if (!vis && titleTaps >= 8) {
            st.setRecVisible(true); pad.setRecVisible(true); titleTaps = 0;
            try { TouchGamepadView.nativeSetFps(st.fps()); } catch (UnsatisfiedLinkError ignored) { }
            Toast.makeText(act, "REC button shown", Toast.LENGTH_SHORT).show();
            build();
        } else if (vis && titleTaps >= 3) {
            st.setRecVisible(false); pad.setRecVisible(false); titleTaps = 0;
            try { TouchGamepadView.nativeSetFps(30); } catch (UnsatisfiedLinkError ignored) { }
            Toast.makeText(act, "REC button hidden", Toast.LENGTH_SHORT).show();
            build();
        }
    }

    private void pickDriver() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try { act.startActivityForResult(i, REQ_DRIVER); }
        catch (Exception e) { Toast.makeText(act, "No file picker available", Toast.LENGTH_SHORT).show(); }
    }

    /** From the activity: the picked driver package. */
    void onDriverPicked(android.net.Uri uri) {
        String name = "driver";
        try (android.database.Cursor c = act.getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int ix = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME);
                if (ix >= 0) name = c.getString(ix);
            }
        } catch (Exception ignored) { }
        final String nm = name;
        Toast.makeText(act, "Installing driver…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String msg;
            try (java.io.InputStream in = act.getContentResolver().openInputStream(uri)) {
                String id = st.installCustom(in, nm);
                st.setDriver(id);
                msg = "Driver installed: " + id.substring(Settings.CUSTOM_PREFIX.length());
                act.runOnUiThread(() -> { needRestart(); build(); });
            } catch (Exception e) {
                msg = "Driver not installed: " + e.getMessage();
            }
            final String m = msg;
            act.runOnUiThread(() -> Toast.makeText(act, m, Toast.LENGTH_LONG).show());
        }).start();
    }

    /** Device, settings, driver status, this run's log, the previous run's
     *  log and any freeze logs, zipped into Download/4thCKR_logs_<time>.zip. */
    private void saveLogs() {
        Toast.makeText(act, "Collecting logs…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String msg;
            try {
                String stamp = new java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US)
                        .format(new java.util.Date());
                String name = "4thCKR_logs_" + stamp + ".zip";
                android.content.ContentValues cv = new android.content.ContentValues();
                cv.put(android.provider.MediaStore.Downloads.DISPLAY_NAME, name);
                cv.put(android.provider.MediaStore.Downloads.MIME_TYPE, "application/zip");
                android.net.Uri uri = act.getContentResolver().insert(
                        android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
                if (uri == null) throw new Exception("cannot create the file in Download");
                try (java.util.zip.ZipOutputStream z = new java.util.zip.ZipOutputStream(
                        act.getContentResolver().openOutputStream(uri))) {
                    StringBuilder info = new StringBuilder();
                    info.append("app ").append(version()).append('\n')
                        .append("device ").append(android.os.Build.MANUFACTURER).append(' ')
                        .append(android.os.Build.MODEL).append(" (").append(android.os.Build.DEVICE).append(")\n")
                        .append("soc ").append(Settings.socName()).append('\n')
                        .append("android ").append(android.os.Build.VERSION.RELEASE)
                        .append(" (API ").append(android.os.Build.VERSION.SDK_INT).append(")\n")
                        .append("driver setting ").append(st.driver()).append('\n')
                        .append("driver status ").append(st.lastDriverStatus()).append('\n');
                    try { info.append("driver in use ").append(TouchGamepadView.nativeDriverActive() != 0 ? "custom" : "system").append('\n'); }
                    catch (Throwable ignored) { }
                    info.append("input devices:\n").append(TouchGamepadView.describeInputDevices());
                    zipText(z, "info.txt", info.toString());
                    Process pr = Runtime.getRuntime().exec(new String[] { "logcat", "-d", "-v", "time" });
                    z.putNextEntry(new java.util.zip.ZipEntry("this_session.txt"));
                    try (java.io.InputStream in = pr.getInputStream()) { copy(in, z); }
                    z.closeEntry();
                    java.io.File dump = new java.io.File(act.getExternalFilesDir(null), "dump");
                    java.io.File[] fs = dump.listFiles();
                    if (fs != null) for (java.io.File f : fs) {
                        String n = f.getName();
                        if (!(n.equals("prev_session.txt") || n.startsWith("hang_") || n.equals("inputs.txt"))) continue;
                        z.putNextEntry(new java.util.zip.ZipEntry(n));
                        try (java.io.InputStream in = new java.io.FileInputStream(f)) { copy(in, z); }
                        z.closeEntry();
                    }
                }
                msg = "Logs saved: Download/" + name;
            } catch (Exception e) {
                msg = "Logs not saved: " + e.getMessage();
            }
            final String m = msg;
            act.runOnUiThread(() -> Toast.makeText(act, m, Toast.LENGTH_LONG).show());
        }).start();
    }

    private static void zipText(java.util.zip.ZipOutputStream z, String name, String text) throws java.io.IOException {
        z.putNextEntry(new java.util.zip.ZipEntry(name));
        z.write(text.getBytes("UTF-8"));
        z.closeEntry();
    }

    private static void copy(java.io.InputStream in, java.io.OutputStream out) throws java.io.IOException {
        byte[] buf = new byte[1 << 16];
        int k;
        while ((k = in.read(buf)) > 0) out.write(buf, 0, k);
    }

    private void restart() {
        Intent i = act.getPackageManager().getLaunchIntentForPackage(act.getPackageName());
        if (i == null) return;
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        android.app.PendingIntent pi = android.app.PendingIntent.getActivity(act, 1, i,
                android.app.PendingIntent.FLAG_CANCEL_CURRENT | android.app.PendingIntent.FLAG_IMMUTABLE);
        android.app.AlarmManager am = (android.app.AlarmManager) act.getSystemService(android.content.Context.ALARM_SERVICE);
        am.set(android.app.AlarmManager.RTC, System.currentTimeMillis() + 400, pi);
        act.finishAffinity();
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    // ---- widgets -----------------------------------------------------------------------------

    private String version() {
        try { return act.getPackageManager().getPackageInfo(act.getPackageName(), 0).versionName; }
        catch (Exception e) { return "?"; }
    }

    private TextView text(String s, float sp, boolean bold) {
        TextView t = new TextView(act);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(CREAM);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private TextView note(String s) {
        TextView t = text(s, 13, false);
        t.setTextColor(CREAM_DIM);
        t.setPadding(0, (int) (2 * dp), 0, (int) (4 * dp));
        return t;
    }

    private void section(String s) {
        TextView t = text(s, 15, true);
        t.setTextColor(AMBER);
        t.setPadding(0, (int) (16 * dp), 0, (int) (4 * dp));
        body.addView(t);
    }

    private RadioGroup group() {
        RadioGroup g = new RadioGroup(act);
        g.setOrientation(RadioGroup.VERTICAL);
        return g;
    }

    private void addRadio(RadioGroup g, int id, String label) {
        RadioButton b = new RadioButton(act);
        b.setId(id);
        b.setText(label);
        b.setTextColor(CREAM);
        b.setTextSize(14);
        b.setButtonTintList(android.content.res.ColorStateList.valueOf(AMBER));
        b.setPadding((int) (4 * dp), (int) (6 * dp), 0, (int) (6 * dp));
        g.addView(b);
    }

    private Button button(String s, View.OnClickListener l) {
        Button b = new Button(act);
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
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, (int) (40 * dp));
        lp.setMargins(0, (int) (6 * dp), (int) (8 * dp), 0);
        b.setLayoutParams(lp);
        b.setOnClickListener(l);
        return b;
    }
}
