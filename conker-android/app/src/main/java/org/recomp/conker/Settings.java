package org.recomp.conker;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** The in-game settings (side menu) and the GPU driver store.
 *  Values that the native side reads at start-up are passed as environment
 *  variables before libmain starts (apply()). */
final class Settings {
    private static final String TAG = "ConkerSettings";

    static final int PAD_AUTO = 0, PAD_ALWAYS = 1, PAD_HIDDEN = 2;
    static final String DRIVER_AUTO = "auto", DRIVER_SYSTEM = "system", CUSTOM_PREFIX = "custom:";
    static final String BUILTIN_ASSET = "gpu_drivers/turnip_mrpurple_T30.zip";
    static final String BUILTIN_ID = "builtin-turnip-T30";

    private final Context ctx;
    private final SharedPreferences p;

    Settings(Context c) {
        ctx = c.getApplicationContext();
        p = ctx.getSharedPreferences("conker_settings", Context.MODE_PRIVATE);
        // once: x2 became the default; a stored x1 from before (or left by a
        // test run) goes back to x2. The settings panel can still pick x1.
        if (!p.getBoolean("scale_default_x2", false))
            p.edit().putInt("scale", 2).putBoolean("scale_default_x2", true).apply();
    }

    int padMode() { return p.getInt("pad_mode", PAD_AUTO); }
    void setPadMode(int m) { p.edit().putInt("pad_mode", m).apply(); }
    int scale() { return p.getInt("scale", 2); }   // x2 by default: sharper
    void setScale(int s) { p.edit().putInt("scale", s).apply(); }
    int sharpness() { return p.getInt("sharpness", 3); }
    void setSharpness(int v) { p.edit().putInt("sharpness", v).apply(); }
    boolean aa() { return p.getBoolean("aa", true); }
    /** Frame-rate cap: 30 (the game's own), 40 or 60 (experimental). */
    int fps() { int f = savedFps(); return recVisible() ? f : 30; }   /* 40/60: developer mode only */
    int savedFps() { int f = p.getInt("fps", 30); return f == 40 || f == 60 ? f : 30; }
    void setFps(int f) { p.edit().putInt("fps", f).apply(); }
    /** This install's network card address (Xbox OUI 00:50:F2), so two
     *  phones are two consoles to System Link. */
    String mac() {
        String m = p.getString("mac", null);
        if (m == null) {
            java.util.Random r = new java.security.SecureRandom();
            m = String.format(java.util.Locale.US, "00:50:f2:%02x:%02x:%02x",
                    r.nextInt(256), r.nextInt(256), r.nextInt(256));
            p.edit().putString("mac", m).apply();
        }
        return m;
    }
    void setAa(boolean v) { p.edit().putBoolean("aa", v).apply(); }
    boolean aspect43() { return p.getBoolean("aspect43", false); }
    /** Text language: "en" (the disc's) or "ru" (the patch in assets/lang_ru). */
    String language() { return p.getString("language", "en"); }
    void setLanguage(String l) { p.edit().putString("language", l).apply(); }
    /** Which disc image was unpacked: "en" or "ru" (the fan Russian one). */
    String imageKind() { return p.getString("image_kind", "en"); }
    void setImageKind(String k) { p.edit().putString("image_kind", k).apply(); }

    /** Copies assets/lang_<code>/ to files/lang_<code>/ once per APK install
     *  and returns that folder, or null. The native side reads files there in
     *  place of the game folder's (CONKER_OVERLAY). */
    private File languageOverlay(String code) {
        File dst = new File(ctx.getExternalFilesDir(null), "lang_" + code);
        File stamp = new File(dst, ".stamp");
        String want;
        try { want = String.valueOf(ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).lastUpdateTime); }
        catch (Exception e) { want = "0"; }
        try {
            String have = stamp.exists() ? new String(java.nio.file.Files.readAllBytes(stamp.toPath())) : "";
            if (!have.equals(want)) {
                copyAssetTree("lang_" + code, dst);
                try (FileOutputStream o = new FileOutputStream(stamp)) { o.write(want.getBytes()); }
            }
            return dst;
        } catch (Exception e) { Log.e(TAG, "language overlay " + code, e); return null; }
    }
    private void copyAssetTree(String asset, File dst) throws java.io.IOException {
        String[] kids = ctx.getAssets().list(asset);
        if (kids == null || kids.length == 0) {          // a file
            dst.getParentFile().mkdirs();
            try (InputStream in = ctx.getAssets().open(asset); OutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[65536]; int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            return;
        }
        dst.mkdirs();
        for (String k : kids) copyAssetTree(asset + "/" + k, new File(dst, k));
    }
    void setAspect43(boolean b) { p.edit().putBoolean("aspect43", b).apply(); }
    boolean recVisible() { return p.getBoolean("rec_visible", false); }
    void setRecVisible(boolean b) { p.edit().putBoolean("rec_visible", b).apply(); }
    String driver() { return p.getString("gpu_driver", DRIVER_AUTO); }
    void setDriver(String d) { p.edit().putString("gpu_driver", d).apply(); }
    boolean builtinFailed() { return p.getBoolean("builtin_failed", false); }

    // ---- GPU ------------------------------------------------------------------------------

    static boolean isSnapdragon() {
        String m = "";
        if (Build.VERSION.SDK_INT >= 31) m = Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL;
        m = (m + " " + Build.HARDWARE + " " + Build.BOARD).toLowerCase();
        return m.contains("qti") || m.contains("qualcomm") || m.contains("qcom");
    }

    static String socName() {
        if (Build.VERSION.SDK_INT >= 31) return Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL;
        return Build.HARDWARE;
    }

    private File gpuDir() { File d = new File(ctx.getFilesDir(), "gpu"); d.mkdirs(); return d; }
    private File pendingFile() { return new File(gpuDir(), "pending.txt"); }

    /** Installed user drivers: folder names under files/gpu/custom. */
    List<String> customDrivers() {
        List<String> out = new ArrayList<>();
        File[] fs = new File(gpuDir(), "custom").listFiles();
        if (fs != null) for (File f : fs) if (f.isDirectory() && libIn(f) != null) out.add(f.getName());
        java.util.Collections.sort(out);
        return out;
    }

    private static File libIn(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return null;
        for (File f : fs) if (f.getName().endsWith(".so")) return f;
        return null;
    }

    /** The driver that this start will use: null = system. */
    File effectiveDriver() {
        String d = driver();
        if (DRIVER_SYSTEM.equals(d)) return null;
        if (d.startsWith(CUSTOM_PREFIX)) return libIn(new File(new File(gpuDir(), "custom"), d.substring(CUSTOM_PREFIX.length())));
        // auto: the built-in Turnip on Snapdragon, unless it failed before
        if (!isSnapdragon() || builtinFailed()) return null;
        return ensureBuiltin();
    }

    private File ensureBuiltin() {
        File dir = new File(gpuDir(), BUILTIN_ID);
        File lib = libIn(dir);
        if (lib != null) return lib;
        try (InputStream in = ctx.getAssets().open(BUILTIN_ASSET)) {
            return unpack(in, dir);
        } catch (Exception e) {
            Log.e(TAG, "built-in driver", e);
            return null;
        }
    }

    /** Unpacks a driver package (.zip with a .so, or a bare .so) into dir. */
    static File unpack(InputStream in, File dir) throws Exception {
        dir.mkdirs();
        File lib = null;
        try (ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            byte[] buf = new byte[1 << 16];
            while ((e = z.getNextEntry()) != null) {
                String n = new File(e.getName()).getName();
                if (e.isDirectory() || n.isEmpty() || !(n.endsWith(".so") || n.endsWith(".json"))) continue;
                File f = new File(dir, n);
                try (OutputStream o = new FileOutputStream(f)) {
                    int k;
                    while ((k = z.read(buf)) > 0) o.write(buf, 0, k);
                }
                if (n.endsWith(".so")) { f.setReadable(true, true); lib = f; }
            }
        }
        if (lib == null) throw new Exception("no driver library (.so) in the package");
        return lib;
    }

    /** Installs a user package; returns its id ("custom:<name>"). */
    String installCustom(InputStream in, String name) throws Exception {
        String safe = name.replaceAll("\\.zip$", "").replaceAll("[^A-Za-z0-9._-]", "_");
        if (safe.isEmpty()) safe = "driver";
        File dir = new File(new File(gpuDir(), "custom"), safe);
        deleteTree(dir);
        try {
            unpack(in, dir);
        } catch (Exception e) {
            deleteTree(dir);
            throw e;
        }
        return CUSTOM_PREFIX + safe;
    }

    void removeCustom(String id) {
        if (!id.startsWith(CUSTOM_PREFIX)) return;
        deleteTree(new File(new File(gpuDir(), "custom"), id.substring(CUSTOM_PREFIX.length())));
        if (id.equals(driver())) setDriver(DRIVER_AUTO);
    }

    private static void deleteTree(File f) {
        File[] fs = f.listFiles();
        if (fs != null) for (File c : fs) deleteTree(c);
        f.delete();
    }

    /** A start with a custom driver that never reached a frame: back to the
     *  system driver. Returns a message for the user, or null. */
    String checkLastStart() {
        File pf = pendingFile();
        if (!pf.exists()) return null;
        String what = "";
        try { what = new String(java.nio.file.Files.readAllBytes(pf.toPath())).trim(); } catch (Exception ignored) { }
        pf.delete();
        if (what.startsWith(BUILTIN_ID)) p.edit().putBoolean("builtin_failed", true).apply();
        else setDriver(DRIVER_SYSTEM);
        return "The GPU driver did not start last time - switched to the system driver.";
    }

    /** The first frame is on screen: the driver works. */
    void markStarted() { pendingFile().delete(); }

    /** Environment for libmain, set before it starts. */
    void apply(String nativeLibDir) {
        setenv("CONKER_SET_SCALE", String.valueOf(scale()));
        if (aspect43()) setenv("CONKER_43", "1");
        setenv("CONKER_SHARPNESS", String.valueOf(sharpness()));
        setenv("CONKER_AA", aa() ? "1" : "0");
        setenv("CONKER_FPS", String.valueOf(fps()));
        setenv("CONKER_MAC", mac());
        if (!"en".equals(language())) {
            File ov = languageOverlay(language());
            if (ov != null) setenv("CONKER_OVERLAY", ov.getAbsolutePath());
        }
        File drv = effectiveDriver();
        if (drv != null) {
            setenv("NV2A_ADRENO_DRIVER_DIR", drv.getParentFile().getAbsolutePath() + "/");
            setenv("NV2A_ADRENO_DRIVER_NAME", drv.getName());
            setenv("NV2A_ADRENO_HOOK_DIR", nativeLibDir + "/");
            try (FileOutputStream o = new FileOutputStream(pendingFile())) {
                o.write(drv.getParentFile().getName().getBytes());
            } catch (Exception ignored) { }
        }
        Log.i(TAG, "scale x" + scale() + ", 4:3 " + aspect43() + ", driver " + (drv != null ? drv : "system"));
    }

    private static void setenv(String k, String v) {
        try { android.system.Os.setenv(k, v, true); } catch (Exception e) { Log.e(TAG, "setenv " + k, e); }
    }
}
