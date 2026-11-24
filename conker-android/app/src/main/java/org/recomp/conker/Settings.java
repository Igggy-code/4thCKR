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

    /** On-screen controller layout set by the player (TouchGamepadView): "" = defaults. */
    String padLayout() { return p.getString("pad_layout_v1", ""); }
    void setPadLayout(String v) { p.edit().putString("pad_layout_v1", v).apply(); }
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
    void setDriver(String d) { p.edit().putString("gpu_driver", d).putString("driver_failed_id", "").apply(); }
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
        File meta = new File(dir, "meta.json");
        if (meta.isFile()) {
            try {
                org.json.JSONObject j = new org.json.JSONObject(new String(
                        java.nio.file.Files.readAllBytes(meta.toPath()), "UTF-8"));
                String n = j.optString("libraryName", "");
                if (!n.isEmpty() && new File(dir, n).isFile()) return new File(dir, n);
            } catch (Exception e) { Log.w(TAG, "driver meta.json", e); }
        }
        File[] fs = dir.listFiles();
        if (fs == null) return null;
        java.util.Arrays.sort(fs);
        for (File f : fs) if (f.getName().endsWith(".so")) return f;
        return null;
    }

    /** The driver that this start will use: null = system. */
    File effectiveDriver() {
        String d = driver();
        if (DRIVER_SYSTEM.equals(d)) return null;
        if (d.startsWith(CUSTOM_PREFIX)) return libIn(new File(new File(gpuDir(), "custom"), d.substring(CUSTOM_PREFIX.length())));
        // auto: the built-in Turnip on Snapdragon, unless it failed before
        // files/nv2a_test_turnip (a test switch): the built-in driver on any GPU,
        // to exercise the loader hooks on a phone without Adreno
        boolean test = new File(ctx.getExternalFilesDir(null), "nv2a_test_turnip").exists();
        if ((!isSnapdragon() && !test) || (builtinFailed() && !test)) return null;
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

    /** Unpacks a driver package (.zip with a .so, or a bare .so) into dir.
     *  The package is copied first and read through its central directory
     *  (ZipFile): ZipInputStream refuses some archives emulator drivers come
     *  in (stored entries with data descriptors). */
    static File unpack(InputStream in, File dir) throws Exception {
        dir.mkdirs();
        File tmp = new File(dir, ".incoming");
        try (OutputStream o = new FileOutputStream(tmp)) {
            byte[] buf = new byte[1 << 16];
            int k;
            while ((k = in.read(buf)) > 0) o.write(buf, 0, k);
        }
        try {
            byte[] head = new byte[4];
            try (InputStream h = new java.io.FileInputStream(tmp)) { h.read(head); }
            if (head[0] == 0x7f && head[1] == 'E' && head[2] == 'L' && head[3] == 'F') {
                File lib = new File(dir, "vulkan.custom.so");
                if (!tmp.renameTo(lib)) throw new Exception("cannot store the driver");
                lib.setReadable(true, true);
                return lib;
            }
            if (head[0] != 'P' || head[1] != 'K')
                throw new Exception("not a driver package (.zip with a .so inside, or a .so)");
            File lib = null;
            try (java.util.zip.ZipFile z = new java.util.zip.ZipFile(tmp)) {
                java.util.Enumeration<? extends ZipEntry> en = z.entries();
                byte[] buf = new byte[1 << 16];
                while (en.hasMoreElements()) {
                    ZipEntry e = en.nextElement();
                    String n = new File(e.getName()).getName();
                    if (e.isDirectory() || n.isEmpty() || !(n.endsWith(".so") || n.endsWith(".json"))) continue;
                    File f = new File(dir, n);
                    try (InputStream zi = z.getInputStream(e); OutputStream o = new FileOutputStream(f)) {
                        int k;
                        while ((k = zi.read(buf)) > 0) o.write(buf, 0, k);
                    }
                    if (n.endsWith(".so")) { f.setReadable(true, true); if (lib == null) lib = f; }
                }
            }
            File named = libIn(dir);
            if (named != null) lib = named;
            if (lib == null) throw new Exception("no driver library (.so) in the package");
            return lib;
        } finally {
            tmp.delete();
        }
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

    private File statusFile() { return new File(gpuDir(), "driver_status.txt"); }

    /** What the native side said about the driver at the last start (all lines). */
    String lastDriverStatus() {
        try {
            List<String> ls = java.nio.file.Files.readAllLines(statusFile().toPath());
            return String.join("; ", ls);
        } catch (Exception e) { return ""; }
    }

    /** A start with a custom driver that never reached a frame (it crashed or
     *  hung): back to the system driver. Returns a message, or null. The
     *  marker holds the process id: a second onCreate in the same process
     *  (the activity recreated) is not a failed start. */
    String checkLastStart() {
        int ver = 0;
        try { ver = (int) ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).getLongVersionCode(); }
        catch (Exception ignored) { }
        if (p.getInt("builtin_failed_ver", -1) != ver) {
            // A new version tries again the driver an earlier start gave up
            // on: the built-in Turnip, and a custom driver the fallback had
            // replaced with "System". Versions before 0.7.4 switched to
            // System without remembering why (and their Turnip could never
            // load), so that one setting goes back to Auto once.
            SharedPreferences.Editor e = p.edit()
                    .putBoolean("builtin_failed", false).putInt("builtin_failed_ver", ver);
            String failed = p.getString("driver_failed_id", "");
            if (DRIVER_SYSTEM.equals(driver())) {
                if (!failed.isEmpty()) e.putString("gpu_driver", failed);
                else if (!p.getBoolean("migrated_074", false)) e.putString("gpu_driver", DRIVER_AUTO);
            }
            e.putString("driver_failed_id", "").putBoolean("migrated_074", true).apply();
        }
        File pf = pendingFile();
        if (!pf.exists()) return null;
        String what = "";
        try { what = new String(java.nio.file.Files.readAllBytes(pf.toPath())).trim(); } catch (Exception ignored) { }
        String[] parts = what.split(" ");
        if (parts.length > 1 && parts[1].equals(String.valueOf(android.os.Process.myPid()))) return null;
        pf.delete();
        String id = parts[0];
        String reason = lastDriverStatus();
        // One start without a frame is not enough to drop a driver: the
        // first start with a new driver builds its shader cache behind a
        // black screen, and closing the app then is no crash. Two in a row is.
        int misses = p.getInt("driver_misses", 0) + 1;
        Log.w(TAG, "driver " + id + " did not reach a frame (" + misses + " in a row); last status: " + reason);
        if (misses < 2) {
            p.edit().putInt("driver_misses", misses).apply();
            return null;
        }
        p.edit().putInt("driver_misses", 0).apply();
        if (id.startsWith(BUILTIN_ID)) p.edit().putBoolean("builtin_failed", true).apply();
        else p.edit().putString("driver_failed_id", driver()).putString("gpu_driver", DRIVER_SYSTEM).apply();
        return "The GPU driver " + id + " did not start twice in a row - switched to the system driver."
                + (reason.isEmpty() ? "" : "\n" + reason);
    }

    /** The first frame is on screen: the driver works. */
    void markStarted() { pendingFile().delete(); p.edit().putInt("driver_misses", 0).apply(); }

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
        statusFile().delete();
        setenv("NV2A_DRIVER_STATUS", statusFile().getAbsolutePath());
        File drv = effectiveDriver();
        if (drv != null) {
            setenv("NV2A_ADRENO_DRIVER_DIR", drv.getParentFile().getAbsolutePath() + "/");
            setenv("NV2A_ADRENO_DRIVER_NAME", drv.getName());
            setenv("NV2A_ADRENO_HOOK_DIR", nativeLibDir + "/");
            try (FileOutputStream o = new FileOutputStream(pendingFile())) {
                o.write((drv.getParentFile().getName() + " " + android.os.Process.myPid()).getBytes());
            } catch (Exception ignored) { }
        }
        Log.i(TAG, "scale x" + scale() + ", 4:3 " + aspect43() + ", driver " + (drv != null ? drv : "system"));
    }

    private static void setenv(String k, String v) {
        try { android.system.Os.setenv(k, v, true); } catch (Exception e) { Log.e(TAG, "setenv " + k, e); }
    }
}
