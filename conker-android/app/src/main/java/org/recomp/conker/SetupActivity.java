package org.recomp.conker;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/** First start: the player picks their Conker disc image (an .iso, plain XISO
 *  or a full Redump image), the files are unpacked into files/game, and the
 *  text language follows the image (the fan Russian disc turns the built-in
 *  Russian patch on). Started again from the settings to change the image. */
public final class SetupActivity extends Activity {
    private static final String TAG = "ConkerSetup";
    static final String EXTRA_FORCE = "force";
    private static final int REQ_ISO = 7;
    private static final int SECTOR = 2048;
    private static final long[] PARTITIONS = { 0L, 0x18300000L, 0x2080000L, 0xFD90000L };
    /** dvddata/aid/font/ConkerFont/default.bin of the fan Russian image. */
    private static final String RU_FONT_MD5 = "0449a5ffa35a6b5395d3870bbca27555";

    private TextView status, progressLine, fileLine, pick;
    private LauncherUi.Bar bar;
    private LinearLayout progressBox;
    private volatile boolean busy;

    static File gameDir(Activity a) { return new File(a.getExternalFilesDir(null), "game"); }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        boolean force = getIntent().getBooleanExtra(EXTRA_FORCE, false);
        if (!force && new File(gameDir(this), "default.xbe").exists()) { launchGame(); return; }

        android.widget.FrameLayout root = new android.widget.FrameLayout(this);
        root.addView(new LauncherUi.Backdrop(this));
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setGravity(Gravity.CENTER);
        int pad = LauncherUi.dp(this, 32);
        l.setPadding(pad, pad / 2, pad, pad / 2);
        android.widget.FrameLayout.LayoutParams lp = new android.widget.FrameLayout.LayoutParams(
                Math.min(LauncherUi.dp(this, 640), getResources().getDisplayMetrics().widthPixels),
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT, Gravity.CENTER);
        android.widget.ScrollView sv = new android.widget.ScrollView(this);
        sv.setFillViewport(true);
        sv.setVerticalScrollBarEnabled(false);
        sv.addView(l, new android.widget.FrameLayout.LayoutParams(
                android.widget.FrameLayout.LayoutParams.MATCH_PARENT,
                android.widget.FrameLayout.LayoutParams.WRAP_CONTENT));
        root.addView(sv, lp);

        /* the project logo instead of the title text */
        android.widget.ImageView logo = new android.widget.ImageView(this);
        logo.setImageResource(R.drawable.logo_4thckr);
        logo.setAdjustViewBounds(true);
        logo.setScaleType(android.widget.ImageView.ScaleType.FIT_CENTER);
        logo.setContentDescription("4thCKR Reported");
        LinearLayout.LayoutParams llp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LauncherUi.dp(this, 150));
        llp.bottomMargin = LauncherUi.dp(this, 14);
        l.addView(logo, llp);

        status = LauncherUi.text(this, "Выберите образ диска Conker (.iso) – английский или русский (фанатский).\n"
                + "Файлы распаковываются один раз, нужно около 5 ГБ свободного места.\n"
                + "Choose your Conker disc image (.iso): English or the Russian fan image.", 15, LauncherUi.MUTED, false);
        status.setLineSpacing(0, 1.15f);
        l.addView(status);

        progressBox = new LinearLayout(this);
        progressBox.setOrientation(LinearLayout.VERTICAL);
        progressBox.setPadding(0, LauncherUi.dp(this, 28), 0, 0);
        bar = new LauncherUi.Bar(this);
        progressBox.addView(bar);
        progressLine = LauncherUi.text(this, "", 15, LauncherUi.CREAM, false);
        progressLine.setPadding(0, LauncherUi.dp(this, 12), 0, 0);
        progressBox.addView(progressLine);
        fileLine = LauncherUi.text(this, "", 12, 0xFF9C8569, false);
        fileLine.setSingleLine(true);
        fileLine.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        progressBox.addView(fileLine);
        progressBox.setVisibility(android.view.View.GONE);
        l.addView(progressBox, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));

        pick = LauncherUi.button(this, "Выбрать ISO  ·  Choose ISO");
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        bp.topMargin = LauncherUi.dp(this, 32);
        bp.gravity = Gravity.CENTER_HORIZONTAL;
        pick.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            startActivityForResult(i, REQ_ISO);
        });
        l.addView(pick, bp);
        setContentView(root);
        getWindow().getDecorView().setSystemUiVisibility(android.view.View.SYSTEM_UI_FLAG_FULLSCREEN
                | android.view.View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | android.view.View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != REQ_ISO || res != RESULT_OK || data == null || data.getData() == null || busy) return;
        final Uri uri = data.getData();
        busy = true;
        pick.setVisibility(android.view.View.GONE);
        progressBox.setVisibility(android.view.View.VISIBLE);
        status.setText("Распаковка образа… Не закрывайте приложение.\nUnpacking the disc image…");
        bar.set(0f);
        new Thread(() -> {
            String err = null;
            boolean ru = false;
            try { ru = extract(uri); }
            catch (Exception e) { Log.e(TAG, "extract", e); err = e.getMessage(); }
            final String fe = err; final boolean fru = ru;
            runOnUiThread(() -> {
                busy = false;
                if (fe != null) {
                    status.setText("Не удалось распаковать образ / Could not unpack the image:\n" + fe);
                    progressBox.setVisibility(android.view.View.GONE);
                    pick.setVisibility(android.view.View.VISIBLE);
                    return;
                }
                Settings st = new Settings(this);
                st.setImageKind(fru ? "ru" : "en");
                if (fru) { st.setLanguage("ru"); launchGame(); return; }
                askLanguage(st);   /* the English disc takes our translation too */
            });
        }, "iso-extract").start();
    }

    /* The English image: the Russian translation (assets/lang_ru) is laid
     * over it as well, so the player picks the language here (and can change
     * it later in the settings). */
    private void askLanguage(Settings st) {
        progressBox.setVisibility(android.view.View.GONE);
        status.setText("Образ распакован. Выберите язык текста игры.\n"
                + "Image unpacked. Choose the game's text language.");
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        TextView ru = LauncherUi.button(this, "Русский");
        TextView en = LauncherUi.button(this, "English");
        LinearLayout.LayoutParams bp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        bp.setMargins(LauncherUi.dp(this, 12), LauncherUi.dp(this, 16), LauncherUi.dp(this, 12),
                LauncherUi.dp(this, 8));
        row.addView(ru, bp);
        row.addView(en, new LinearLayout.LayoutParams(bp));
        ((LinearLayout) status.getParent()).addView(row);
        ru.setOnClickListener(v -> { st.setLanguage("ru"); launchGame(); });
        en.setOnClickListener(v -> { st.setLanguage("en"); launchGame(); });
    }

    private void launchGame() {
        startActivity(new Intent(this, ConkerActivity.class));
        finish();
    }

    // ---- XDVDFS ----------------------------------------------------------

    private static final class Entry {
        final String path; final long sector; final long size;
        Entry(String p, long s, long n) { path = p; sector = s; size = n; }
    }

    private long lastUi;
    private FileChannel ch;
    private long base;

    private ByteBuffer read(long pos, int n) throws IOException {
        ByteBuffer bb = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN);
        while (bb.hasRemaining()) {
            int r = ch.read(bb, pos + bb.position());
            if (r < 0) break;
        }
        bb.flip();
        return bb;
    }

    private void walk(long sector, long size, String prefix, List<Entry> out) throws IOException {
        if (size == 0) return;
        ByteBuffer d = read(base + sector * SECTOR, (int) size);
        java.util.ArrayDeque<Integer> stack = new java.util.ArrayDeque<>();
        java.util.HashSet<Integer> seen = new java.util.HashSet<>();
        stack.push(0);
        while (!stack.isEmpty()) {
            int off = stack.pop();
            if (!seen.add(off) || off + 14 > d.limit()) continue;
            int left = d.getShort(off) & 0xFFFF, right = d.getShort(off + 2) & 0xFFFF;
            if (left == 0xFFFF && right == 0xFFFF) continue;
            long start = d.getInt(off + 4) & 0xFFFFFFFFL, fsize = d.getInt(off + 8) & 0xFFFFFFFFL;
            int attr = d.get(off + 12) & 0xFF, nlen = d.get(off + 13) & 0xFF;
            byte[] nb = new byte[nlen];
            for (int k = 0; k < nlen && off + 14 + k < d.limit(); k++) nb[k] = d.get(off + 14 + k);
            String name = new String(nb, java.nio.charset.StandardCharsets.ISO_8859_1);
            if ((attr & 0x10) != 0) walk(start, fsize, prefix + name + "/", out);
            else out.add(new Entry(prefix + name, start, fsize));
            if (left != 0 && left != 0xFFFF) stack.push(left * 4);
            if (right != 0 && right != 0xFFFF) stack.push(right * 4);
        }
    }

    /** Unpacks the image into files/game; returns true for the Russian fan image. */
    private boolean extract(Uri uri) throws Exception {
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r");
             FileInputStream fis = new FileInputStream(pfd.getFileDescriptor())) {
            ch = fis.getChannel();
            base = -1;
            byte[] magic = "MICROSOFT*XBOX*MEDIA".getBytes("ISO-8859-1");
            for (long off : PARTITIONS) {
                ByteBuffer m = read(off + 32L * SECTOR, 20);
                byte[] got = new byte[m.remaining()]; m.get(got);
                if (java.util.Arrays.equals(got, magic)) { base = off; break; }
            }
            if (base < 0) throw new IOException("not an Xbox disc image");
            ByteBuffer r = read(base + 32L * SECTOR + 20, 8);
            long rootSector = r.getInt(0) & 0xFFFFFFFFL, rootSize = r.getInt(4) & 0xFFFFFFFFL;
            List<Entry> files = new ArrayList<>();
            walk(rootSector, rootSize, "", files);
            boolean hasXbe = false;
            long total = 0;
            for (Entry e : files) { total += e.size; if (e.path.equalsIgnoreCase("default.xbe")) hasXbe = true; }
            if (!hasXbe) throw new IOException("default.xbe not found in the image");

            File dst = gameDir(this);
            File tmp = new File(getExternalFilesDir(null), "game.unpacking");
            deleteTree(tmp);
            tmp.mkdirs();
            long done = 0;
            boolean ru = false;
            byte[] buf = new byte[1 << 20];
            for (Entry e : files) {
                File f = new File(tmp, e.path);
                f.getParentFile().mkdirs();
                boolean font = e.path.equalsIgnoreCase("dvddata/aid/font/ConkerFont/default.bin");
                MessageDigest md = font ? MessageDigest.getInstance("MD5") : null;
                try (FileOutputStream o = new FileOutputStream(f)) {
                    long pos = base + e.sector * SECTOR, left = e.size;
                    while (left > 0) {
                        int n = (int) Math.min(left, buf.length);
                        ByteBuffer bb = ByteBuffer.wrap(buf, 0, n);
                        int got = 0;
                        while (bb.hasRemaining()) { int k = ch.read(bb, pos + got); if (k < 0) break; got += k; }
                        if (got <= 0) throw new IOException("short read: " + e.path);
                        o.write(buf, 0, got);
                        if (md != null) md.update(buf, 0, got);
                        pos += got; left -= got; done += got;
                        long now = System.currentTimeMillis();
                        if (now - lastUi > 100) {
                            lastUi = now;
                            final float p = done / (float) Math.max(1, total);
                            final String line = String.format(java.util.Locale.US, "%d%%  ·  %.1f / %.1f ГБ",
                                    (int) (p * 100), done / 1e9, total / 1e9);
                            final String name = e.path;
                            runOnUiThread(() -> { bar.set(p); progressLine.setText(line); fileLine.setText(name); });
                        }
                    }
                }
                if (md != null) {
                    StringBuilder sb = new StringBuilder();
                    for (byte x : md.digest()) sb.append(String.format("%02x", x));
                    ru = RU_FONT_MD5.equals(sb.toString());
                }
            }
            // saves kept in the old game folder (UDATA/TDATA) move over to the new one
            for (String keep : new String[] { "UDATA", "TDATA" }) {
                File old = new File(dst, keep);
                if (old.exists()) { deleteTree(new File(tmp, keep)); old.renameTo(new File(tmp, keep)); }
            }
            deleteTree(dst);
            if (!tmp.renameTo(dst)) throw new IOException("could not move the unpacked files");
            Log.i(TAG, "unpacked " + files.size() + " files, " + total + " bytes, russian=" + ru);
            return ru;
        }
    }

    static void deleteTree(File f) {
        if (!f.exists()) return;
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }
}
