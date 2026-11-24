package org.recomp.conker;

import android.content.Intent;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.ViewGroup;
import android.widget.Toast;
import org.libsdl.app.SDLActivity;

/** SDL hosts the window, audio and game controllers; the recompiled title
 *  lives in libmain.so. The on-screen controller and the settings menu sit
 *  above SDL's surface. */
public class ConkerActivity extends SDLActivity implements TouchGamepadView.MenuHost {
    private TouchGamepadView gamepadView;
    private SettingsPanel panel;
    private Settings settings;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /** The last run's log (a crash, a kill) is still in the log buffer:
     *  keep it as dump/prev_session.txt before it rolls away. */
    private void savePreviousSession() {
        final android.content.SharedPreferences sp = getSharedPreferences("session", MODE_PRIVATE);
        final int old = sp.getInt("pid", 0);
        sp.edit().putInt("pid", android.os.Process.myPid()).apply();
        if (old == android.os.Process.myPid()) return;
        final java.io.File dir = new java.io.File(getExternalFilesDir(null), "dump");
        new Thread(() -> {
            try {
                dir.mkdirs();
                Process pr = Runtime.getRuntime().exec(new String[] { "logcat", "-d", "-v", "time", "-t", "80000" });
                // the app sees only its own logs: keep all but this run's
                int me = android.os.Process.myPid();
                String self = "(" + me + ")", self2 = "(" + String.format(java.util.Locale.US, "%5d", me) + ")";
                try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(pr.getInputStream()));
                     java.io.PrintWriter out = new java.io.PrintWriter(new java.io.File(dir, "prev_session.txt"))) {
                    String l;
                    while ((l = in.readLine()) != null)
                        if (!l.contains(self) && !l.contains(self2)) out.println(l);
                }
            } catch (Exception e) { android.util.Log.e("ConkerActivity", "prev session", e); }
            saveExitReasons(dir);
        }).start();
    }

    /** Why the game's earlier processes ended (crash, low memory, the title
     *  relaunching itself...), as the system keeps it: survives a cleared
     *  log. dump/exits.txt; a native crash's tombstone as dump/tombstone_<time>.pb. */
    private void saveExitReasons(java.io.File dir) {
        if (android.os.Build.VERSION.SDK_INT < 30) return;
        try {
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            java.util.List<android.app.ApplicationExitInfo> l = am.getHistoricalProcessExitReasons(null, 0, 16);
            java.text.SimpleDateFormat f = new java.text.SimpleDateFormat("MM-dd HH:mm:ss", java.util.Locale.US);
            StringBuilder sb = new StringBuilder();
            for (android.app.ApplicationExitInfo x : l) {
                String line = f.format(new java.util.Date(x.getTimestamp())) + " pid " + x.getPid()
                        + " " + x.getProcessName() + " reason " + x.getReason() + " status " + x.getStatus()
                        + " importance " + x.getImportance() + " pss " + x.getPss() / 1024 + " MB rss "
                        + x.getRss() / 1024 + " MB: " + x.getDescription();
                sb.append(line).append('\n');
                android.util.Log.i("ConkerExit", line);
                if (x.getReason() == android.app.ApplicationExitInfo.REASON_CRASH_NATIVE
                        || x.getReason() == android.app.ApplicationExitInfo.REASON_ANR) {
                    java.io.File t = new java.io.File(dir, (x.getReason() == android.app.ApplicationExitInfo.REASON_ANR
                            ? "anr_" : "tombstone_") + x.getTimestamp() + ".pb");
                    if (t.exists()) continue;
                    try (java.io.InputStream in = x.getTraceInputStream()) {
                        if (in == null) continue;
                        try (java.io.FileOutputStream out = new java.io.FileOutputStream(t)) {
                            byte[] b = new byte[65536]; int n;
                            while ((n = in.read(b)) > 0) out.write(b, 0, n);
                        }
                    }
                }
            }
            try (java.io.PrintWriter out = new java.io.PrintWriter(new java.io.File(dir, "exits.txt"))) {
                out.print(sb);
            }
        } catch (Exception e) { android.util.Log.e("ConkerActivity", "exit reasons", e); }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Settings the native side reads at start-up: before libmain runs.
        settings = new Settings(this);
        savePreviousSession();
        try {   // System Link: Wi-Fi drops broadcasts to sleeping apps without this
            android.net.wifi.WifiManager wm = (android.net.wifi.WifiManager)
                    getApplicationContext().getSystemService(WIFI_SERVICE);
            if (wm != null) {
                android.net.wifi.WifiManager.MulticastLock ml = wm.createMulticastLock("conker-systemlink");
                ml.setReferenceCounted(false);
                ml.acquire();
            }
        } catch (Exception e) { android.util.Log.w("ConkerActivity", "multicast lock", e); }
        String fallback = settings.checkLastStart();
        settings.apply(getApplicationInfo().nativeLibraryDir);
        super.onCreate(savedInstanceState);
        ViewGroup layout = (ViewGroup) getContentView();
        if (layout != null && !mBrokenLibraries) {
            gamepadView = new TouchGamepadView(this);
            gamepadView.setMenuHost(this);
            gamepadView.setPadMode(settings.padMode());
            gamepadView.setRecVisible(settings.recVisible());
            gamepadView.setAspect43(settings.aspect43());
            layout.addView(gamepadView, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            panel = new SettingsPanel(this, settings, gamepadView);
            layout.addView(panel, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        if (fallback != null) Toast.makeText(this, fallback, Toast.LENGTH_LONG).show();
        // test switch files/nv2a_test_layout: open the layout editor after 20 s (screenshots)
        java.io.File ext = getExternalFilesDir(null);
        if (layout != null && gamepadView != null && ext != null && new java.io.File(ext, "nv2a_test_layout").exists()) {
            final ViewGroup root = layout;
            new android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                    () -> {
                        ControlsLayoutBar.open(this, root, gamepadView);
                        if (new java.io.File(ext, "nv2a_test_layout_always").exists())
                            ControlsLayoutBar.testLayer(TouchGamepadView.LAYER_ALWAYS);
                    }, 20000);
        }
    }

    @Override public void openMenu() { if (panel != null) panel.show(); }

    @Override public void firstFrame() { if (settings != null) settings.markStarted(); }

    @Override
    public void onBackPressed() {
        if (ControlsLayoutBar.closeIfOpen()) return;
        if (panel != null) panel.toggle();
    }

    private static boolean fromPad(int source) {
        return (source & android.view.InputDevice.SOURCE_GAMEPAD) == android.view.InputDevice.SOURCE_GAMEPAD
            || (source & android.view.InputDevice.SOURCE_JOYSTICK) == android.view.InputDevice.SOURCE_JOYSTICK;
    }

    @Override
    public boolean dispatchGenericMotionEvent(android.view.MotionEvent event) {
        if (fromPad(event.getSource())) TouchGamepadView.padUsed();
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (fromPad(event.getSource()) && event.getKeyCode() != KeyEvent.KEYCODE_BACK) TouchGamepadView.padUsed();
        // Back (the system button / gesture, not a gamepad's B) opens the menu.
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && panel != null
                && (event.getSource() & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD) {
            if (event.getAction() == KeyEvent.ACTION_UP && !event.isCanceled()
                    && !ControlsLayoutBar.closeIfOpen()) panel.toggle();
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == SettingsPanel.REQ_DRIVER && res == RESULT_OK && data != null && data.getData() != null
                && panel != null)
            panel.onDriverPicked(data.getData());
    }

    /** Called from the game thread when the title ends (see main.c
     *  xbox_title_exit_hook); the process exits right after. A relaunch hands
     *  over to RestartActivity in its own process, which opens the game again
     *  once this process is gone. */
    public void titleExited(boolean relaunch) {
        if (!relaunch) return;
        try {
            Intent i = new Intent(this, RestartActivity.class);
            i.putExtra("pid", android.os.Process.myPid());
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
        } catch (Exception e) { android.util.Log.e("ConkerActivity", "relaunch", e); }
    }
}
