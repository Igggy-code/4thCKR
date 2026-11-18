package org.recomp.conker;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Shader;
import android.graphics.Paint;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen controller, taken from the 4DevilTears (DMC4) port and adapted to
 * the original Xbox pad: LB/RB are the White/Black buttons. Presses go to
 * libmain, which merges them into pad 0.
 *
 * Layout (landscape), mirroring the 360 pad:
 *   LT LB          BACK [pad] START          RB RT
 *   (left stick)                     Y
 *        [D-pad]                   X   B
 *                     (right stick)  A
 * The left and right halves of the screen are "floating" stick zones: touch
 * anywhere there (outside the buttons) and the stick centres under the finger.
 * Double-tap a stick zone and hold to click the stick (L3 / R3).
 * The small pad icon at the top hides / shows the overlay; it hides itself
 * automatically while a physical controller is connected.
 */
public class TouchGamepadView extends View implements InputManager.InputDeviceListener {
    private static final String TAG = "ConkerTouch";

    // SDL_GamepadButton
    static final int BTN_A = 0, BTN_B = 1, BTN_X = 2, BTN_Y = 3, BTN_BACK = 4, BTN_START = 6,
            BTN_LS = 7, BTN_RS = 8, BTN_LB = 9, BTN_RB = 10,
            BTN_UP = 11, BTN_DOWN = 12, BTN_LEFT = 13, BTN_RIGHT = 14;
    // SDL_GamepadAxis
    static final int AX_LX = 0, AX_LY = 1, AX_RX = 2, AX_RY = 3, AX_LT = 4, AX_RT = 5;

    static native boolean nativeAttach();
    static native void nativeButton(int button, boolean down);
    static native void nativeAxis(int axis, float value);
    static native float nativeFps();
    static native int nativeMark();
    static native void nativeSetDisplayFx(int sharpness, int aa);
    static native void nativeSetFps(int fps);
    static native int nativeCapture();
    static native int nativeLabel(int state);
    static native int nativeGameState();
    static native int nativeDriverActive();
    static native int nativeMenuKind();
    static native int nativePresented();
    static native int nativeAimMode();
    // front-end menu: 0 none, 1 menu (left / right), 2 Chapters list (up / down)
    private int menuKind = 0;
    private boolean aspect43;
    void setAspect43(boolean a) { aspect43 = a; }

    // ---- settings (side menu) ----
    interface MenuHost { void openMenu(); void firstFrame(); }
    private MenuHost menuHost;
    private int padMode = Settings.PAD_AUTO;
    private boolean recVisible = false, menuOpen = false, firstFrameSeen = false;
    void setMenuHost(MenuHost h) { menuHost = h; }
    void setPadMode(int m) { padMode = m; revealUntil = 0; if (m == Settings.PAD_ALWAYS) { cinemaHidden = false; updateVisibility(); } invalidate(); }
    void setRecVisible(boolean v) { recVisible = v; invalidate(); }
    void setMenuOpen(boolean o) { menuOpen = o; if (o) releaseAll(); }
    // edge swipe opens the menu
    private int edgePid = -1;
    private float edgeX, edgeY;

    // List menus (uListSelect subclasses) by vtable: touch rows in 1280x720 game space.
    // { vtable, x0, x1, half row height, row centre y... } - rows in cursor order.
    private static final int[][] MENU_ROWS = {
        { 0x820A8C70, 220, 640, 24, 502, 551, 600 },                 // mission start
        { 0x8208E508, 200, 640, 19, 128, 168, 408, 448, 528, 608 },  // mission menu
        { 0x820AA1F8, 250, 540, 26, 287, 342, 398, 453, 509 },       // pause menu
    };
    // Title main menu (New Game / Load Game / Options), title state 7.
    private static final int[] TITLE_ROWS = { 0, 500, 780, 21, 517, 563, 608 };
    // Menus where the on-screen controller is hidden (touch / gestures instead).
    private static final int[] KNOWN_MENUS = { 0x820A8C70, 0x8208E508, 0x820A7FA8, 0x820AA1F8 };
    private int listVt = 0;

    private static boolean knownMenu(int vt) {
        for (int k : KNOWN_MENUS) if (k == vt) return true;
        return false;
    }

    private static int[] rowsFor(int vt) {
        for (int[] r : MENU_ROWS) if (r[0] == vt) return r;
        return null;
    }

    /** Screen -> 1280x720 game coordinates for the current picture scaling. */
    private float[] toGame(float x, float y) {
        float w = getWidth(), h = getHeight();
        float sx, sy;
        if ("stretch".equals(S_SCALE)) { sx = w / 1280f; sy = h / 720f; }
        else if ("fill".equals(S_SCALE)) { sx = sy = Math.max(w / 1280f, h / 720f); }
        else { sx = sy = Math.min(w / 1280f, h / 720f); }
        float ox = (w - 1280f * sx) / 2f, oy = (h - 720f * sy) / 2f;
        return new float[] { (x - ox) / sx, (y - oy) / sy };
    }

    private static final String S_SCALE = "fit";
    private static boolean S_SHOW_FPS = false;
    private static boolean sNativeOk = true;

    /* Every button press, with ms since start, to <ext files>/dump/inputs.txt:
     * a route recorded by hand, replayed by the automated tests. */
    private static java.io.File sInputLog;
    private static final long sT0 = android.os.SystemClock.elapsedRealtime();
    private static void logButton(int b, boolean down) {
        if (sInputLog == null || !down) return;
        try (java.io.FileWriter w = new java.io.FileWriter(sInputLog, true)) {
            w.write((android.os.SystemClock.elapsedRealtime() - sT0) + " " + b + "\n");
        } catch (java.io.IOException e) { /* best effort */ }
    }

    private static void button(int b, boolean down) {
        logButton(b, down);
        if (!sNativeOk) return;
        try { nativeButton(b, down); } catch (UnsatisfiedLinkError e) { sNativeOk = false; Log.e(TAG, "native", e); }
    }

    private static void axis(int a, float v) {
        if (!sNativeOk) return;
        try { nativeAxis(a, v); } catch (UnsatisfiedLinkError e) { sNativeOk = false; Log.e(TAG, "native", e); }
    }

    // ---- controls -------------------------------------------------------------------------

    private static final int K_BUTTON = 0, K_TRIGGER = 1, K_DPAD = 2, K_LSTICK = 3, K_RSTICK = 4, K_TOGGLE = 5;

    private static class Ctl {
        final int kind, id;
        final String label;
        final int color;
        float cx, cy, r;           // circle (buttons, d-pad, stick base)
        RectF rect;                // rounded rect (shoulders, triggers, back/start)
        boolean pressed;
        Ctl(int kind, int id, String label, int color) { this.kind = kind; this.id = id; this.label = label; this.color = color; }
        boolean hit(float x, float y, float slop) {
            if (rect != null) {
                return x >= rect.left - slop && x <= rect.right + slop && y >= rect.top - slop && y <= rect.bottom + slop;
            }
            float dx = x - cx, dy = y - cy, rr = r + slop;
            return dx * dx + dy * dy <= rr * rr;
        }
    }

    private static class Stick {
        final int axX, axY, click;
        float homeX, homeY, baseX, baseY, knobX, knobY, radius;
        int pointer = -1;
        long lastDown;
        boolean clicked;
        Stick(int axX, int axY, int click) { this.axX = axX; this.axY = axY; this.click = click; }
    }

    private final Ctl[] ctls;
    private final Ctl dpad, toggle;
    private final Stick lstick = new Stick(AX_LX, AX_LY, BTN_LS);
    private final Stick rstick = new Stick(AX_RX, AX_RY, BTN_RS);
    private final SparseArray<Object> owners = new SparseArray<>();   // pointer id -> Ctl / Stick
    private int dpadMask;                                             // bits: up down left right

    private final float dp;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);

    private boolean overlayVisible = true;
    private boolean userHidden = false;
    // Cinema mode: hidden (nothing drawn at all) while the game plays a cutscene / movie.
    private boolean cinemaHidden = true;   // clean picture at launch, until the game is playable
    private int sceneMode = 0;
    private int menuState = 0;
    private int gameState = 0;   // native: 0 gameplay, 1 cutscene/loading, 3 menu
    // files/state.txt for the automated tests: what the game shows right now.
    private int lastScene = -1, lastMenu = -1, cutscenes = 0, movies = 0;

    // Intro movie auto-skip: the first movie of the session, half a second after it starts.
    private long introSeenAt = 0;
    private boolean introDone = false;

    private void maybeSkipIntro() {
        if (introDone || !false) return;
        if (sceneMode != 2) {
            if (introSeenAt != 0) introDone = true;  // it ended on its own
            return;
        }
        long now = System.currentTimeMillis();
        if (introSeenAt == 0) { introSeenAt = now; return; }
        if (now - introSeenAt >= 500) {
            introDone = true;
            button(BTN_START, true);
            handler.postDelayed(() -> button(BTN_START, false), 120);
        }
    }

    private void publishState() {
        if (sceneMode == lastScene && menuState == lastMenu) return;
        if (sceneMode == 1 && lastScene != 1) cutscenes++;
        if (sceneMode == 2 && lastScene != 2) movies++;
        lastScene = sceneMode;
        lastMenu = menuState;
        String line = "scene=" + sceneMode + " title=" + ((menuState & 0x10000) != 0 ? ((menuState >> 8) & 0xFF) : -1)
                + " menu=" + Integer.toHexString(listVt)
                + " cutscenes=" + cutscenes + " movies=" + movies + "\n";
        try (java.io.FileOutputStream f = new java.io.FileOutputStream(
                new java.io.File(getContext().getFilesDir(), "state.txt"))) {
            f.write(line.getBytes());
        } catch (Exception ignored) {
        }
    }
    // Menu gestures (title screen for now): tap = confirm, swipe = D-pad, two fingers = back.
    private float gestX, gestY;
    private int gestPid = -1;
    // Touch-to-skip: first tap shows the hint, a second tap within the window presses START.
    private long hintShownAt = 0, hintUntil = 0;
    private static final long HINT_MS = 3000;
    private final Paint hintPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hintBand = new Paint();
    // Idle fade (settings: opacity / fade when idle).
    private long lastTouch = System.currentTimeMillis();
    private long toggleDownTime;

    /** From the settings: hide / show the on-screen controller. */
    public void setUserHidden(boolean hidden) {
        userHidden = hidden;
        updateVisibility();
    }
    private final Handler handler = new Handler(Looper.getMainLooper());
    private InputManager inputManager;

    public TouchGamepadView(Context context) {
        super(context);
        dp = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(2f * dp);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);

        Ctl a = new Ctl(K_BUTTON, BTN_A, "A", 0xFF5DBB3F);
        Ctl b = new Ctl(K_BUTTON, BTN_B, "B", 0xFFE0453A);
        Ctl x = new Ctl(K_BUTTON, BTN_X, "X", 0xFF3A7BE0);
        Ctl y = new Ctl(K_BUTTON, BTN_Y, "Y", 0xFFF2C12E);
        Ctl lb = new Ctl(K_BUTTON, BTN_LB, "WH", Color.WHITE);
        Ctl rb = new Ctl(K_BUTTON, BTN_RB, "BK", 0xFFAAAAAA);
        Ctl lt = new Ctl(K_TRIGGER, AX_LT, "L", Color.WHITE);
        Ctl rt = new Ctl(K_TRIGGER, AX_RT, "R", Color.WHITE);
        Ctl back = new Ctl(K_BUTTON, BTN_BACK, "BACK", Color.WHITE);
        Ctl start = new Ctl(K_BUTTON, BTN_START, "START", Color.WHITE);
        dpad = new Ctl(K_DPAD, 0, "", Color.WHITE);
        toggle = new Ctl(K_TOGGLE, 0, "◐", Color.WHITE);
        // TEMP (diagnostics): label the game state for the memory snapshots.
        Ctl cut = new Ctl(K_BUTTON, 101, "CUT", 0xFFFF9800);
        Ctl game = new Ctl(K_BUTTON, 102, "GAME", 0xFF00E5FF);
        ctls = new Ctl[] { a, b, x, y, lb, rb, lt, rt, back, start, dpad, toggle };

        setFocusable(false);
        handler.post(attachPoll);
        handler.postDelayed(fpsPoll, 1000);
    }

    // FPS counter (guest frames per second, published by the GPU backend).
    private float fps = -1f;
    private final Paint fpsPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Runnable fpsPoll = new Runnable() {
        @Override public void run() {
            if (sNativeOk) {
                try { fps = nativeFps(); } catch (UnsatisfiedLinkError e) { fps = -1f; }
                if (fps > 0 && !firstFrameSeen) { firstFrameSeen = true; if (menuHost != null) menuHost.firstFrame(); }
                int gs = 0;
                try { gs = nativeGameState(); } catch (UnsatisfiedLinkError e) { gs = 0; }
                if (gs != 0 && gameState == 0 && padMode != Settings.PAD_ALWAYS) {
                    // gameplay just ended (a cutscene, the pause): let go of
                    // what is held, no lingering reveal
                    revealUntil = 0;
                    if (owners.size() > 0) releaseAll();
                }
                gameState = gs;
                int am = 0;
                try { am = nativeAimMode(); } catch (UnsatisfiedLinkError e) { am = 0; }
                boolean aim = am != 0 && gs == 0 && padMode != Settings.PAD_HIDDEN;
                if (aim != aimMode) {
                    // aiming: the camera stick moves under the left thumb (the
                    // right one is free for the buttons); back when it ends
                    if (lstick.pointer != -1) releaseStick(lstick);
                    if (rstick.pointer != -1) releaseStick(rstick);
                    aimMode = aim;
                    applyAimLayout();
                    resetStick(rstick);
                    invalidate();
                }
                int mk = 0;
                try { mk = nativeMenuKind(); } catch (UnsatisfiedLinkError e) { mk = 0; }
                int pres = 0;
                try { pres = nativePresented(); } catch (UnsatisfiedLinkError e) { pres = 1; }
                boolean ld = pres == 0;      // only while nothing is on screen yet: the game draws its own load screens
                if (ld != loadingNow) { loadingNow = ld; invalidate(); }
                if (mk != menuKind) {
                    menuKind = mk;
                    if (mk == 3 && padMode != Settings.PAD_ALWAYS) {
                        // pause: the controls go at once, and anything held
                        // is let go (a held stick or button kept them up)
                        releaseAll();
                        revealUntil = 0;
                    }
                    invalidate();
                }
                // Everything outside gameplay (cutscenes, loading, menus) takes
                // gestures: tap = A, hold = START, swipe = D-pad, two fingers = B.
                sceneMode = 0;
                menuState = gs != 0 ? 0x20000 : 0;
                listVt = 0;
                publishState();
                maybeSkipIntro();
                // Controls only in gameplay: cutscenes, loading and menus show a
                // clean picture (tap twice to skip / swipes and taps in menus).
                // (a gesture in progress no longer brings the pad back: in menus
                // every swipe flashed the controls)
                boolean ch = (sceneMode != 0 || menuState != 0)
                        && (cinemaHidden || owners.size() == 0)
                        && System.currentTimeMillis() > revealUntil;
                if (menuKind == 3 || menuKind >= 4) ch = true;   // 4: loading screen, 5: a movie
                if (padMode == Settings.PAD_ALWAYS) ch = false;
                else if (padMode == Settings.PAD_HIDDEN) ch = true;
                if (ch != cinemaHidden) {
                    cinemaHidden = ch;
                    if (!ch) hintUntil = 0;
                    updateVisibility();
                }
                pollInject();
                updateAlpha(false);
                invalidate();
            }
            handler.postDelayed(this, 100);   // the pause screen reacts within a frame or three
        }
    };

    /* Automated tests: a file inject_<BUTTON> in the app's external files dir
     * presses that button once (A, B, X, Y, START, BACK, UP, DOWN, LEFT,
     * RIGHT), whatever the overlay shows. */
    private void pollInject() {
        java.io.File dir = getContext().getExternalFilesDir(null);
        if (dir == null) return;
        java.io.File cf = new java.io.File(dir, "inject_CAPTURE");
        if (cf.exists() && cf.delete()) doCapture();
        String[] names = { "A", "B", "X", "Y", "START", "BACK", "UP", "DOWN", "LEFT", "RIGHT" };
        int[] btns = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_START, BTN_BACK, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
        for (int i = 0; i < names.length; i++) {
            java.io.File f = new java.io.File(dir, "inject_" + names[i]);
            if (f.exists() && f.delete()) {
                Log.i(TAG, "inject " + names[i]);
                pulse(btns[i]);
            }
        }
        // inject_RSTICK_<dir>: the camera stick held that way for 0.6 s
        for (int i = 0; i < 6; i++) {
            String[] rd = { "UP", "DOWN", "LEFT", "RIGHT", "UPLEFT", "UPRIGHT" };
            float[][] rv = { {0,-1}, {0,1}, {-1,0}, {1,0}, {-0.7f,-0.7f}, {0.7f,-0.7f} };
            java.io.File f = new java.io.File(dir, "inject_RSTICK_" + rd[i]);
            if (f.exists() && f.delete()) {
                Log.i(TAG, "inject rstick " + rd[i]);
                axis(AX_RX, rv[i][0]); axis(AX_RY, rv[i][1]);
                handler.postDelayed(new Runnable() { @Override public void run() {
                    axis(AX_RX, 0); axis(AX_RY, 0); } }, 1500);
            }
        }
        // inject_STICK_<dir>: the left stick held that way for 3 s
        // (UP, DOWN, LEFT, RIGHT, UPLEFT, UPRIGHT)
        String[] sd = { "UP", "DOWN", "LEFT", "RIGHT", "UPLEFT", "UPRIGHT" };
        float[][] sv = { {0,-1}, {0,1}, {-1,0}, {1,0}, {-0.7f,-0.7f}, {0.7f,-0.7f} };
        for (int i = 0; i < sd.length; i++) {
            java.io.File f = new java.io.File(dir, "inject_STICK_" + sd[i]);
            if (f.exists() && f.delete()) {
                Log.i(TAG, "inject stick " + sd[i]);
                final float x = sv[i][0], y = sv[i][1];
                axis(AX_LX, x); axis(AX_LY, y);
                handler.postDelayed(new Runnable() { @Override public void run() {
                    axis(AX_LX, 0); axis(AX_LY, 0); } }, 3000);
            }
        }
    }

    private void drawFps(Canvas canvas) {
        if (fps < 0 || !S_SHOW_FPS) return;
        fpsPaint.setTextAlign(Paint.Align.CENTER);
        fpsPaint.setFakeBoldText(true);
        fpsPaint.setTextSize(13 * dp);
        fpsPaint.setColor(fps >= 25 ? 0xCC7CFC00 : (fps >= 15 ? 0xCCFFD700 : 0xCCFF5050));
        canvas.drawText(String.format(java.util.Locale.US, "%.1f FPS", fps),
                toggle.cx, toggle.cy + toggle.r + 16 * dp, fpsPaint);
    }

    private final Runnable attachPoll = new Runnable() {
        @Override public void run() {
            boolean ok = false;
            try { ok = nativeAttach(); } catch (UnsatisfiedLinkError e) { sNativeOk = false; Log.e(TAG, "native", e); return; }
            if (!ok) handler.postDelayed(this, 500);
            else Log.i(TAG, "virtual gamepad ready");
        }
    };

    // ---- physical controller detection ----------------------------------------------------

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        java.io.File ext = getContext().getExternalFilesDir(null);
        if (ext != null) {
            java.io.File d = new java.io.File(ext, "dump");
            d.mkdirs();
            sInputLog = new java.io.File(d, "inputs.txt");
            sInputLog.delete();
        }
        inputManager = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(this, handler);
        updateVisibility();
    }

    @Override protected void onDetachedFromWindow() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        handler.removeCallbacks(attachPoll);
        handler.removeCallbacks(fpsPoll);
        releaseAll();
        super.onDetachedFromWindow();
    }

    @Override public void onInputDeviceAdded(int id) { updateVisibility(); }
    @Override public void onInputDeviceRemoved(int id) { updateVisibility(); }
    @Override public void onInputDeviceChanged(int id) { updateVisibility(); }

    private static boolean physicalPadConnected() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                return true;
            }
        }
        return false;
    }

    private void updateVisibility() {
        boolean v = !userHidden && !cinemaHidden && !physicalPadConnected();
        if (v != overlayVisible) {
            overlayVisible = v;
            if (!v) releaseAll();
            invalidate();
        }
    }

    // ---- layout -----------------------------------------------------------------------------

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        // Sizes in dp; scale down a bit on short screens.
        // Designed for ~840x400 dp landscape; shrink uniformly to fit anything smaller
        // (including portrait / split-screen) so controls never overlap.
        float s = Math.min(1f, Math.min((w / dp) / 840f, (h / dp) / 400f)) * dp;
        float face = 27 * s, faceOff = 52 * s;
        float fcx = w - 125 * s, fcy = h - 175 * s;
        place(ctls[0], fcx, fcy + faceOff, face);   // A
        place(ctls[1], fcx + faceOff, fcy, face);   // B
        place(ctls[2], fcx - faceOff, fcy, face);   // X
        place(ctls[3], fcx, fcy - faceOff, face);   // Y

        float top = 14 * s, bw = 92 * s, bh = 42 * s, gap = 10 * s, side = 18 * s;
        ctls[6].rect = new RectF(side, top, side + bw, top + bh);                         // LT
        ctls[4].rect = new RectF(side + bw + gap, top + 18 * s, side + 2 * bw + gap, top + 18 * s + bh); // LB
        ctls[7].rect = new RectF(w - side - bw, top, w - side, top + bh);                 // RT
        ctls[5].rect = new RectF(w - side - 2 * bw - gap, top + 18 * s, w - side - bw - gap, top + 18 * s + bh); // RB

        float sw = 78 * s, sh = 34 * s;
        ctls[8].rect = new RectF(w / 2f - 60 * s - sw, top, w / 2f - 60 * s, top + sh);   // BACK
        ctls[9].rect = new RectF(w / 2f + 60 * s, top, w / 2f + 60 * s + sw, top + sh);   // START
        if (ctls.length > 12) ctls[12].rect = new RectF(w / 2f - 60 * s - sw, top + sh + 14 * s, w / 2f - 60 * s, top + 2 * sh + 14 * s); // CUT (temp)
        if (ctls.length > 13) ctls[13].rect = new RectF(w / 2f + 60 * s, top + sh + 14 * s, w / 2f + 60 * s + sw, top + 2 * sh + 14 * s); // GAME (temp)
        place(toggle, w / 2f, top + sh / 2f, 18 * s);

        place(dpad, 275 * s, h - 80 * s, 62 * s);

        lstick.radius = 62 * s;
        lstick.homeX = 125 * s; lstick.homeY = h - 175 * s;
        rstick.radius = 55 * s;
        rstick.homeX = w - 280 * s; rstick.homeY = h - 80 * s;
        rHomeX = rstick.homeX; rHomeY = rstick.homeY; rRadius = rstick.radius;
        applyAimLayout();
        resetStick(lstick);
        resetStick(rstick);

        text.setTextSize(15 * s);
    }

    private static void place(Ctl c, float x, float y, float r) { c.cx = x; c.cy = y; c.r = r; c.rect = null; }

    private void resetStick(Stick st) {
        st.baseX = st.knobX = st.homeX;
        st.baseY = st.knobY = st.homeY;
    }

    // ---- input ------------------------------------------------------------------------------

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                float x = e.getX(idx), y = e.getY(idx);
                lastTouch = System.currentTimeMillis();
                if (menuOpen) return true;
                if (recVisible && recHit(x, y)) { doCapture(); return true; }
                if (action == MotionEvent.ACTION_DOWN && (x < 18 * dp || x > getWidth() - 18 * dp)) {
                    // a swipe in from the edge opens the settings
                    edgePid = e.getPointerId(idx); edgeX = x; edgeY = y;
                    return true;
                }
                if (cinemaHidden && !userHidden && !physicalPadConnected()) {
                    // Left side: an invisible floating stick. The state guess
                    // (gameplay or not) is not always right -- in-engine scenes
                    // can hand control back without the usual signs -- so moving
                    // the stick always works, and brings the controller back.
                    Stick fs = aimMode ? rstick : lstick;
                    if (menuKind == 0 && x < getWidth() * 0.42f && fs.pointer == -1 && gestPid == -1) {
                        int pid = e.getPointerId(idx);
                        fs.pointer = pid;
                        owners.put(pid, fs);
                        fs.baseX = x; fs.baseY = y; fs.knobX = x; fs.knobY = y;
                        return true;
                    }
                    if (sceneMode == 0 && menuState != 0) menuTouchDown(e.getPointerId(idx), x, y, action);
                    else cinemaTouch(e.getPointerId(idx), x, y);
                    return true;
                }
                updateAlpha(true);
                if (!overlayVisible) return false;  // let the game see it
                down(e.getPointerId(idx), x, y);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                if (menuOpen) return true;
                if (edgePid != -1) {
                    int ei = e.findPointerIndex(edgePid);
                    if (ei >= 0) {
                        float dx = e.getX(ei) - edgeX, dy = e.getY(ei) - edgeY;
                        boolean inward = edgeX < getWidth() / 2f ? dx > 48 * dp : dx < -48 * dp;
                        if (inward && Math.abs(dx) > 1.5f * Math.abs(dy)) {
                            edgePid = -1;
                            if (menuHost != null) menuHost.openMenu();
                        }
                    }
                }
                if (revealUntil != 0 && !cinemaHidden)
                    revealUntil = Math.max(revealUntil, System.currentTimeMillis() + 3000);
                for (int i = 0; i < e.getPointerCount(); i++) move(e.getPointerId(i), e.getX(i), e.getY(i));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                if (e.getPointerId(idx) == edgePid) { edgePid = -1; break; }
                if (e.getPointerId(idx) == gestPid) {
                    menuTouchUp(e.getX(idx), e.getY(idx));
                    break;
                }
                up(e.getPointerId(idx));
                break;
            case MotionEvent.ACTION_CANCEL:
                releaseAll();
                break;
        }
        invalidate();
        return overlayVisible;
    }

    // ---- menus without the controller: tap = confirm, swipe = D-pad, two fingers = back -----

    private void menuTouchDown(int pid, float x, float y, int action) {
        if (action == MotionEvent.ACTION_POINTER_DOWN && gestPid != -1) {
            gestPid = -1;  // second finger: back
            pulse(BTN_B);
            return;
        }
        gestPid = pid;
        gestX = x;
        gestY = y;
        gestT = System.currentTimeMillis();
    }
    private long gestT;
    private long revealUntil;
    private boolean aimMode;
    private float rHomeX, rHomeY, rRadius;
    private void applyAimLayout() {
        if (rRadius == 0) return;
        if (aimMode) {
            rstick.homeX = lstick.homeX; rstick.homeY = lstick.homeY; rstick.radius = lstick.radius;
        } else {
            rstick.homeX = rHomeX; rstick.homeY = rHomeY; rstick.radius = rRadius;
        }
    }

    private void menuTouchUp(float x, float y) {
        gestPid = -1;
        float dx = x - gestX, dy = y - gestY;
        float slop = 28 * dp;
        if (Math.abs(dx) < slop && Math.abs(dy) < slop && pauseTap(gestX, gestY)) {
            return;
        }
        if (Math.abs(dx) < slop && Math.abs(dy) < slop && hintTap(gestX, gestY)) {
            return;
        }
        if (Math.abs(dx) < slop && Math.abs(dy) < slop && menuTapRow(x, y)) {
            return;
        }
        if (Math.abs(dx) < slop && Math.abs(dy) < slop) {
            // Tap: A (dialogue, menus). Hold: START (title screen, skip a cutscene).
            boolean hold = System.currentTimeMillis() - gestT >= 600;
            pulse(hold ? BTN_START : BTN_A);
        } else if (Math.abs(dy) >= Math.abs(dx)) {
            pulse(dy < 0 ? BTN_UP : BTN_DOWN);
        } else {
            // some screens (Game N: Play / Erase) read only the stick sideways
            final float v = dx < 0 ? -1f : 1f;
            axis(AX_LX, v);
            pulse(dx < 0 ? BTN_LEFT : BTN_RIGHT);
            handler.postDelayed(() -> axis(AX_LX, 0f), 130);
        }
    }

    /** Tap on a row of a known list menu: on the selected row - confirm (A); on another row -
     *  move the cursor there with D-pad presses (the next tap confirms). */
    private boolean menuTapRow(float x, float y) {
        boolean title = (menuState & 0x10000) != 0;
        int[] rows = title ? (((menuState >> 8) & 0xFF) == 7 ? TITLE_ROWS : null) : rowsFor(listVt);
        if (rows == null) return false;
        float[] g = toGame(x, y);
        if (g[0] < rows[1] || g[0] > rows[2]) return true;  // outside the list: ignore
        int target = -1;
        for (int i = 4; i < rows.length; i++) {
            if (Math.abs(g[1] - rows[i]) <= rows[3]) { target = i - 4; break; }
        }
        if (target < 0) return true;
        int cur;
        cur = -1;
        if (cur < 0 || cur > 64) return false;
        if (cur == target) { pulse(BTN_A); return true; }
        final int btn = target > cur ? BTN_DOWN : BTN_UP;
        int steps = Math.abs(target - cur);
        for (int i = 0; i < steps; i++) {
            handler.postDelayed(() -> { button(btn, true); }, i * 160L);
            handler.postDelayed(() -> { button(btn, false); }, i * 160L + 90);
        }
        haptic();
        return true;
    }

    private void pulse(int btn) {
        button(btn, true);
        haptic();
        handler.postDelayed(() -> button(btn, false), 110);
    }

    // ---- cinema mode: clean picture, touch to skip ------------------------------------------

    private void cinemaTouch(int pid, float x, float y) {
        long now = System.currentTimeMillis();
        // START and A keep working as invisible hotspots (the automated tests tap them, and
        // they are where a player's thumbs already are).
        Ctl start = ctls[9], a = ctls[0];
        Ctl hot = start.hit(x, y, 8 * dp) ? start : (a.hit(x, y, 8 * dp) ? a : null);
        if (hot != null) {
            owners.put(pid, hot);
            press(hot, true);
            hintUntil = 0;
        } else if (now < hintUntil) {
            hintUntil = 0;
            haptic();
            button(BTN_START, true);
            handler.postDelayed(() -> button(BTN_START, false), 120);
        } else {
            hintShownAt = now;
            hintUntil = now + HINT_MS;
        }
        invalidate();
    }

    private void drawSkipHint(Canvas canvas) {
        long now = System.currentTimeMillis();
        if (now >= hintUntil) return;
        float t = (now - hintShownAt) / 1000f, left = (hintUntil - now) / 1000f;
        float a = Math.min(1f, Math.min(t / 0.25f, left / 0.5f));
        int w = getWidth(), h = getHeight();
        float bandTop = h * 0.70f;
        hintBand.setShader(new LinearGradient(0, bandTop, 0, h, 0x00000000,
                ((int) (0xB0 * a) << 24), Shader.TileMode.CLAMP));
        canvas.drawRect(0, bandTop, w, h, hintBand);
        hintPaint.setTextAlign(Paint.Align.CENTER);
        hintPaint.setColor(((int) (0xF0 * a) << 24) | 0xFFFFFF);
        hintPaint.setTextSize(17 * dp);
        hintPaint.setFakeBoldText(true);
        hintPaint.setLetterSpacing(0.08f);
        String what = sceneMode == 2 ? "MOVIE" : "CUTSCENE";
        canvas.drawText("TAP AGAIN TO SKIP " + what, w / 2f, h - 40 * dp, hintPaint);
        // Thin progress line: how long the second tap is accepted.
        float frac = Math.max(0f, left / (HINT_MS / 1000f));
        hintPaint.setStrokeWidth(2 * dp);
        hintPaint.setColor(((int) (0x90 * a) << 24) | 0xE0453A);
        float lw = 140 * dp * frac;
        canvas.drawLine(w / 2f - lw / 2f, h - 28 * dp, w / 2f + lw / 2f, h - 28 * dp, hintPaint);
        postInvalidateOnAnimation();
    }

    // ---- opacity / idle fade ------------------------------------------------------------------

    private boolean isOptional(Ctl c) {
        // Single player needs neither the D-pad nor BACK (multiplayer stats):
        // hidden and inactive. Injected presses (tests) still work.
        // White / Black (WH, BK) are unused too.
        // The hide-the-overlay toggle (◐) is gone too: hiding is a setting
        // (Back -> Settings -> On-screen controller -> Off).
        return c == ctls[8] || c == dpad || c == ctls[4] || c == ctls[5] || c == toggle;
    }

    private void updateAlpha(boolean touched) {
        float base = 0.85f;
        boolean idle = true && owners.size() == 0
                && System.currentTimeMillis() - lastTouch > 4000;
        float target = cinemaHidden ? 1f : base * (idle && !touched ? 0.35f : 1f);
        if (Math.abs(getAlpha() - target) < 0.01f) return;
        animate().cancel();
        if (touched) setAlpha(target);
        else animate().alpha(target).setDuration(target < getAlpha() ? 700 : 150).start();
    }

    /** Settings changed: re-apply opacity / layout. */
    public void refreshFromSettings() {
        updateAlpha(true);
        invalidate();
    }

    private void down(int pid, float x, float y) {
        float slop = 8 * dp;
        for (Ctl c : ctls) {
            if (c.kind == K_DPAD || isOptional(c) || !c.hit(x, y, slop)) continue;
            if (c.kind == K_TOGGLE) {
                // Short tap hides the overlay, holding it opens the settings.
                toggleDownTime = System.currentTimeMillis();
                owners.put(pid, c);
                return;
            }
            owners.put(pid, c);
            press(c, true);
            return;
        }
        if (!isOptional(dpad) && dpad.hit(x, y, slop)) {
            owners.put(pid, dpad);
            dpad.pressed = true;
            setDpad(x, y);
            return;
        }
        Stick st = x < getWidth() / 2f ? (aimMode ? rstick : lstick) : (aimMode ? null : rstick);
        if (st == null || st.pointer != -1) return;
        st.pointer = pid;
        owners.put(pid, st);
        long now = System.currentTimeMillis();
        if (now - st.lastDown < 300) { st.clicked = true; button(st.click, true); }
        st.lastDown = now;
        // Floating stick: centre under the finger.
        st.baseX = x; st.baseY = y; st.knobX = x; st.knobY = y;
        haptic();
    }

    private void move(int pid, float x, float y) {
        Object o = owners.get(pid);
        if (o == dpad) {
            setDpad(x, y);
        } else if (o instanceof Stick) {
            Stick st = (Stick) o;
            float dx = x - st.baseX, dy = y - st.baseY;
            float len = (float) Math.hypot(dx, dy);
            if (len > st.radius) {
                if (st == lstick) {
                    // Movement stick: the base stays where the finger first
                    // touched; past the rim it is simply full deflection, the
                    // knob resting on the rim.
                    dx = dx / len * st.radius; dy = dy / len * st.radius;
                } else {
                    // Drag the base along so the stick never "sticks" at the rim.
                    float over = len - st.radius;
                    st.baseX += dx / len * over;
                    st.baseY += dy / len * over;
                    dx = x - st.baseX; dy = y - st.baseY;
                }
            }
            st.knobX = st.baseX + dx; st.knobY = st.baseY + dy;
            if (st == lstick && len > 24 * dp && menuKind == 0 && gameState == 0) {
                // the player is moving: show the controller, and keep it a while.
                // Not in cutscenes: the stick still works there, unseen, and the
                // pad comes back by itself if the game hands control back.
                revealUntil = System.currentTimeMillis() + 5000;
                if (cinemaHidden) { cinemaHidden = false; hintUntil = 0; updateVisibility(); }
            }
            float nx = dx / st.radius, ny = dy / st.radius;
            float mag = (float) Math.hypot(nx, ny);
            if (mag < 0.08f) { nx = 0; ny = 0; }
            axis(st.axX, nx);
            axis(st.axY, ny);   // SDL: +Y is down, same as the screen
        }
    }

    private void up(int pid) {
        Object o = owners.get(pid);
        owners.remove(pid);
        if (o == dpad) {
            dpad.pressed = false;
            applyDpad(0);
        } else if (o == toggle) {
            long held = System.currentTimeMillis() - toggleDownTime;
            if (held >= 2500) {
                // long hold: memory snapshot for diagnostics (game-state flags)
                int n = -1;
                try { n = nativeMark(); } catch (UnsatisfiedLinkError e) { Log.e(TAG, "native", e); }
                haptic();
                handler.postDelayed(this::haptic, 150);
                android.widget.Toast.makeText(getContext(), "Snapshot " + n, android.widget.Toast.LENGTH_SHORT).show();
            } else if (held >= 500) {
                S_SHOW_FPS = !S_SHOW_FPS;  // hold: FPS counter on/off
            } else {
                userHidden = true;
                updateVisibility();
            }
        } else if (o instanceof Ctl) {
            press((Ctl) o, false);
        } else if (o instanceof Stick) {
            releaseStick((Stick) o);
        }
    }

    private void releaseStick(Stick st) {
        st.pointer = -1;
        if (st.clicked) { st.clicked = false; button(st.click, false); }
        axis(st.axX, 0);
        axis(st.axY, 0);
        resetStick(st);
    }

    private void press(Ctl c, boolean down) {
        if (c.pressed == down) return;
        c.pressed = down;
        if (c.id >= 101) {   // TEMP state labels
            if (down) {
                int n = -1;
                try { n = nativeLabel(c.id - 100); } catch (UnsatisfiedLinkError e) { Log.e(TAG, "native", e); }
                haptic();
                android.widget.Toast.makeText(getContext(), c.label + " #" + n, android.widget.Toast.LENGTH_SHORT).show();
            }
            return;
        }
        if (c.kind == K_TRIGGER) axis(c.id, down ? 1f : 0f);
        else button(c.id, down);
        if (down) haptic();
    }

    private void setDpad(float x, float y) {
        float dx = x - dpad.cx, dy = y - dpad.cy;
        int mask = 0;
        float dead = dpad.r * 0.25f;
        if (Math.hypot(dx, dy) > dead) {
            double ang = Math.toDegrees(Math.atan2(dy, dx));  // 0 = right, 90 = down
            // 8-way with 45-degree sectors.
            if (ang > -67.5 && ang < 67.5) mask |= 8;          // right
            if (ang > 112.5 || ang < -112.5) mask |= 4;        // left
            if (ang > 22.5 && ang < 157.5) mask |= 2;          // down
            if (ang < -22.5 && ang > -157.5) mask |= 1;        // up
        }
        applyDpad(mask);
    }

    private void applyDpad(int mask) {
        if (mask == dpadMask) return;
        int changed = mask ^ dpadMask;
        int[] bits = { 1, 2, 4, 8 };
        int[] btns = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT };
        for (int i = 0; i < 4; i++) {
            if ((changed & bits[i]) != 0) button(btns[i], (mask & bits[i]) != 0);
        }
        if ((mask & ~dpadMask) != 0) haptic();
        dpadMask = mask;
    }

    private void releaseAll() {
        for (Ctl c : ctls) if (c.pressed && c != dpad) press(c, false);
        dpad.pressed = false;
        applyDpad(0);
        if (lstick.pointer != -1) releaseStick(lstick);
        if (rstick.pointer != -1) releaseStick(rstick);
        owners.clear();
    }

    private void haptic() {
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
    }

    // ---- drawing ----------------------------------------------------------------------------

    // TEMP: what the game-state detector sees, top left, to check it in play.
    private final Paint stPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private void drawStateTag(Canvas canvas) {
        stPaint.setTextSize(11 * dp);
        stPaint.setColor(gameState == 0 ? 0xA000E5FF : gameState == 3 ? 0xA0B388FF : 0xA0FF9800);
        canvas.drawText(gameState == 0 ? "GAME" : "CUT/MENU", 8 * dp, 14 * dp, stPaint);
    }

    // ---- menu swipe hints ----------------------------------------------------------------------
    private final Paint mhFill = new Paint(Paint.ANTI_ALIAS_FLAG), mhEdge = new Paint(Paint.ANTI_ALIAS_FLAG),
            mhText = new Paint(Paint.ANTI_ALIAS_FLAG), mhArrow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private static final int MH_CREAM = 0xF4E6C8, MH_GOLD = 0xF2C46B, MH_WOOD = 0x2A170A, MH_EDGE = 0xB07A32;

    // hint parts that work as buttons: tap = that action (UP / DOWN / LEFT / RIGHT / A / B)
    private final java.util.ArrayList<RectF> hintRects = new java.util.ArrayList<>();
    private final java.util.ArrayList<Integer> hintBtns = new java.util.ArrayList<>();
    private RectF hintColumn;

    private void hintPart(RectF r, int btn) { hintRects.add(r); hintBtns.add(btn); }

    /** A tap on a hint: its action. Returns true when it hit one. */
    private boolean hintTap(float x, float y) {
        if (menuKind == 0 || menuKind >= 4 || !cinemaHidden) return false;
        float sl = 6 * dp;
        for (int i = 0; i < hintRects.size(); i++) {
            RectF r = hintRects.get(i);
            if (x >= r.left - sl && x <= r.right + sl && y >= r.top - sl && y <= r.bottom + sl) {
                int b = hintBtns.get(i);
                if (b == BTN_LEFT || b == BTN_RIGHT) {
                    axis(AX_LX, b == BTN_LEFT ? -1f : 1f);
                    handler.postDelayed(() -> axis(AX_LX, 0f), 130);
                }
                pulse(b);
                return true;
            }
        }
        return false;
    }

    private void drawMenuHints(Canvas cv) {
        mhFill.setColor(withAlpha(MH_WOOD, 200));
        mhEdge.setStyle(Paint.Style.STROKE);
        mhEdge.setStrokeWidth(1.5f * dp);
        mhEdge.setColor(withAlpha(MH_EDGE, 220));
        mhText.setTypeface(android.graphics.Typeface.create(android.graphics.Typeface.SERIF, android.graphics.Typeface.BOLD));
        mhText.setTextAlign(Paint.Align.CENTER);
        mhText.setTextSize(14 * dp);
        mhText.setColor(withAlpha(MH_CREAM, 245));
        mhArrow.setColor(withAlpha(MH_GOLD, 250));
        hintRects.clear(); hintBtns.clear(); hintColumn = null;
        float w = getWidth(), h = getHeight();
        // bottom bar: segments, each one a button
        if (menuKind == 3) { drawPauseHint(cv); return; }
        boolean ru = uiRu();
        String sSwipe = ru ? "Свайп  \u00b7  сменить" : "Swipe  \u00b7  choose";
        String sTap = ru ? "Тап  \u00b7  выбрать" : "Tap  \u00b7  select";
        String sBack = ru ? "Два пальца  \u00b7  назад" : "Two fingers  \u00b7  back";
        String[] seg = menuKind == 2 ? new String[] { sTap, sBack } : new String[] { sSwipe, sTap, sBack };
        int[] btn = menuKind == 2 ? new int[] { BTN_A, BTN_B } : new int[] { -1, BTN_A, BTN_B };
        float gap = 28 * dp, arrowW = menuKind == 2 ? 0 : 30 * dp;
        float tw = 0;
        for (String t : seg) tw += mhText.measureText(t);
        tw += gap * (seg.length - 1) + 2 * arrowW;
        float bh = 36 * dp, bw = tw + 36 * dp, bx = (w - bw) / 2f, by = h - bh - 14 * dp;
        RectF b = new RectF(bx, by, bx + bw, by + bh);
        cv.drawRoundRect(b, bh / 2f, bh / 2f, mhFill);
        cv.drawRoundRect(b, bh / 2f, bh / 2f, mhEdge);
        float ty = by + bh / 2f - (mhText.descent() + mhText.ascent()) / 2f, cy = by + bh / 2f;
        float x = bx + 18 * dp;
        if (arrowW > 0) {
            arrow(cv, x + arrowW / 2f, cy, -1, 0);
            hintPart(new RectF(x - 10 * dp, by, x + arrowW, by + bh), BTN_LEFT);
            x += arrowW;
        }
        for (int i = 0; i < seg.length; i++) {
            float sw = mhText.measureText(seg[i]);
            cv.drawText(seg[i], x + sw / 2f, ty, mhText);
            if (btn[i] >= 0) hintPart(new RectF(x - gap / 2f, by, x + sw + gap / 2f, by + bh), btn[i]);
            x += sw + gap;
        }
        if (arrowW > 0) {
            x -= gap;
            arrow(cv, x + arrowW / 2f, cy, 1, 0);
            hintPart(new RectF(x, by, x + arrowW + 10 * dp, by + bh), BTN_RIGHT);
        }
        if (menuKind == 2) {
            // right edge: a column with up / down; the arrows are buttons, a swipe on it moves too
            float cw = 46 * dp, ch = 190 * dp, cx = w - cw - 18 * dp, cyTop = (h - ch) / 2f;
            RectF c = new RectF(cx, cyTop, cx + cw, cyTop + ch);
            cv.drawRoundRect(c, cw / 2f, cw / 2f, mhFill);
            cv.drawRoundRect(c, cw / 2f, cw / 2f, mhEdge);
            float mx = cx + cw / 2f;
            arrow(cv, mx, cyTop + 28 * dp, 0, -1);
            arrow(cv, mx, cyTop + ch - 28 * dp, 0, 1);
            hintPart(new RectF(cx - 8 * dp, cyTop - 8 * dp, cx + cw + 8 * dp, cyTop + 56 * dp), BTN_UP);
            hintPart(new RectF(cx - 8 * dp, cyTop + ch - 56 * dp, cx + cw + 8 * dp, cyTop + ch + 8 * dp), BTN_DOWN);
            hintColumn = c;
            cv.save();
            cv.rotate(-90, mx, cyTop + ch / 2f);
            cv.drawText(ru ? "Свайп" : "Swipe", mx, cyTop + ch / 2f - (mhText.descent() + mhText.ascent()) / 2f, mhText);
            cv.restore();
        }
    }

    /** The on-screen hints follow the game's text language (read once). */
    private int uiRu = -1;
    private boolean uiRu() {
        if (uiRu < 0) uiRu = "ru".equals(new Settings(getContext()).language()) ? 1 : 0;
        return uiRu == 1;
    }

    /** The pause screen: its A / B / Y icons are the buttons (pauseTap). */
    private void drawPauseHint(Canvas cv) {
        float w = getWidth(), h = getHeight();
        String line = uiRu() ? "Нажимайте кнопки на картинке" : "Tap the buttons on the picture";
        float bh = 32 * dp, bw = mhText.measureText(line) + 36 * dp, bx = (w - bw) / 2f, by = h - bh - 10 * dp;
        RectF b = new RectF(bx, by, bx + bw, by + bh);
        cv.drawRoundRect(b, bh / 2f, bh / 2f, mhFill);
        cv.drawRoundRect(b, bh / 2f, bh / 2f, mhEdge);
        cv.drawText(line, w / 2f, by + bh / 2f - (mhText.descent() + mhText.ascent()) / 2f, mhText);
    }

    /** A tap on the pause screen, in the game picture's own coordinates
     *  (0..1 across the stretched image, so any screen size): the A / B / Y
     *  icons under the framed picture. */
    private boolean pauseTap(float x, float y) {
        if (menuKind != 3) return false;
        // the game picture: the whole screen, or a centred 4:3 in that mode
        float pw = aspect43 ? getHeight() * 4f / 3f : getWidth(), px0 = (getWidth() - pw) / 2f;
        float u = (x - px0) / pw, v = y / getHeight();
        for (float[] z : PAUSE_ZONES)
            if (u >= z[0] && u <= z[1] && v >= z[2] && v <= z[3]) { pulse((int) z[4]); return true; }
        return false;
    }
    // u0 u1 v0 v1 button: the Quit? / Resume / Options row under the picture
    private static final float[][] PAUSE_ZONES = {
        { 0.20f, 0.40f, 0.62f, 0.82f, BTN_A },   // Quit?
        { 0.40f, 0.60f, 0.62f, 0.82f, BTN_B },   // Resume
        { 0.60f, 0.84f, 0.60f, 0.80f, BTN_Y },   // Options
    };

    /** A filled chevron triangle pointing (dx, dy). */
    private void arrow(Canvas cv, float x, float y, int dx, int dy) {
        float s = 7 * dp;
        android.graphics.Path p = new android.graphics.Path();
        if (dx != 0) {
            p.moveTo(x + dx * s, y); p.lineTo(x - dx * s * 0.6f, y - s); p.lineTo(x - dx * s * 0.6f, y + s);
        } else {
            p.moveTo(x, y + dy * s); p.lineTo(x - s, y - dy * s * 0.6f); p.lineTo(x + s, y - dy * s * 0.6f);
        }
        p.close();
        cv.drawPath(p, mhArrow);
    }

    // ---- REC: capture the next frames' render log for diagnostics ----------
    private float recX() { return 30 * dp; }
    private float recY() { return 44 * dp; }
    private float recR() { return 16 * dp; }
    private boolean recHit(float x, float y) {
        float dx = x - recX(), dy = y - recY(), r = recR() + 10 * dp;
        return dx * dx + dy * dy <= r * r;
    }
    private long recFlashUntil;
    private void drawRec(Canvas cv) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        boolean flash = System.currentTimeMillis() < recFlashUntil;
        p.setColor(flash ? 0xCCFF3030 : 0x55FF3030);
        cv.drawCircle(recX(), recY(), recR(), p);
        p.setColor(0xCCFFFFFF);
        p.setTextSize(10 * dp);
        p.setTextAlign(Paint.Align.CENTER);
        cv.drawText("REC", recX(), recY() + 3.5f * dp, p);
        if (flash) postInvalidateDelayed(100);
    }
    private void doCapture() {
        int n = -1;
        try { n = nativeCapture(); } catch (UnsatisfiedLinkError e) { Log.e(TAG, "native", e); }
        haptic();
        recFlashUntil = System.currentTimeMillis() + 3000;
        invalidate();
        final int num = n;
        // after the logged frames are out, save this process's log to dump/
        handler.postDelayed(() -> new Thread(() -> {
            String msg;
            try {
                java.io.File dir = new java.io.File(getContext().getExternalFilesDir(null), "dump");
                dir.mkdirs();
                java.io.File f = new java.io.File(dir, String.format(java.util.Locale.US, "capture_%02d.txt", num));
                Process pr = Runtime.getRuntime().exec(new String[] { "logcat", "-d", "-v", "time",
                        "-t", "40000", "--pid=" + android.os.Process.myPid() });
                try (java.io.InputStream in = pr.getInputStream();
                     java.io.OutputStream out = new java.io.FileOutputStream(f)) {
                    byte[] b = new byte[65536]; int k;
                    while ((k = in.read(b)) > 0) out.write(b, 0, k);
                }
                msg = "Capture " + num + " saved";
            } catch (Exception e) {
                Log.e(TAG, "capture", e);
                msg = "Capture failed: " + e.getMessage();
            }
            final String m = msg;
            handler.post(() -> android.widget.Toast.makeText(getContext(), m,
                    android.widget.Toast.LENGTH_SHORT).show());
        }).start(), 3000);
    }

    // Loading bar (bottom of the screen) while the title boots or loads a level.
    private boolean loadingNow = true;
    private android.graphics.Bitmap loadLogo;
    private Paint loadLogoPaint;
    private LauncherUi.Bar loadBar;
    private String loadText;
    private android.text.TextPaint loadPaint;
    private void drawLoading(Canvas cv) {
        if (loadBar == null) {
            loadBar = new LauncherUi.Bar(getContext());
            loadBar.setBlue();
            boolean ru = "ru".equals(new Settings(getContext()).language());
            loadText = ru ? "Загрузка…" : "Loading…";
            loadPaint = new android.text.TextPaint(Paint.ANTI_ALIAS_FLAG);
            loadPaint.setTypeface(LauncherUi.bodyFace(getContext()));
            loadPaint.setTextSize(15 * dp);
            loadPaint.setColor(0xFFCFEFFF);
            loadPaint.setShadowLayer(6 * dp, 0, 0, 0xC0207FB8);
        }
        int bw = (int) Math.min(getWidth() * 0.42f, 420 * dp), bh = (int) (14 * dp);
        loadBar.layout(0, 0, bw, bh);
        float x = (getWidth() - bw) / 2f, y = getHeight() - 44 * dp;
        // the project logo, centred in the space above the bar: half the
        // width at most, under half the height, its own proportions
        if (loadLogo == null) {
            try {
                loadLogo = android.graphics.BitmapFactory.decodeResource(getResources(), R.drawable.logo_4thckr);
            } catch (Throwable t) { loadLogo = null; }
            loadLogoPaint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        }
        if (loadLogo != null && loadLogo.getWidth() > 0) {
            float areaTop = 0, areaBottom = y - 40 * dp;
            float maxW = getWidth() * 0.50f, maxH = (areaBottom - areaTop) * 0.55f;
            float k = Math.min(maxW / loadLogo.getWidth(), maxH / loadLogo.getHeight());
            float lw = loadLogo.getWidth() * k, lh = loadLogo.getHeight() * k;
            float lx = (getWidth() - lw) / 2f, ly = areaTop + (areaBottom - areaTop - lh) / 2f;
            cv.drawBitmap(loadLogo, null, new RectF(lx, ly, lx + lw, ly + lh), loadLogoPaint);
        }
        cv.save();
        cv.translate(x, y);
        loadBar.draw(cv);
        cv.restore();
        float tw = loadPaint.measureText(loadText);
        cv.drawText(loadText, (getWidth() - tw) / 2f, y - 10 * dp, loadPaint);
        postInvalidateOnAnimation();
    }

    @Override protected void onDraw(Canvas canvas) {
        if (loadingNow) drawLoading(canvas);
        if (recVisible) drawStateTag(canvas);
        if (recVisible) drawRec(canvas);
        if (cinemaHidden) {  // clean picture: no controls, no toggle, no FPS
            if (menuKind >= 4) return;             // loading screen / movie: nothing on it
            if (menuKind != 0) { drawMenuHints(canvas); return; }
            drawSkipHint(canvas);
            return;
        }
        drawFps(canvas);
        if (!overlayVisible) return;
        if (!aimMode) drawStick(canvas, lstick);
        drawStick(canvas, rstick);
        for (Ctl c : ctls) {
            if (isOptional(c)) continue;
            if (c == dpad) drawDpad(canvas);
            else if (c.rect != null) drawRect(canvas, c);
            else drawCircle(canvas, c, c.color, true);
        }
    }

    private void drawCircle(Canvas cv, Ctl c, int color, boolean labelColored) {
        fill.setColor(c.pressed ? withAlpha(color, 0xB0) : 0x30000000);
        cv.drawCircle(c.cx, c.cy, c.r, fill);
        stroke.setColor(withAlpha(color, c.pressed ? 0xFF : 0x99));
        cv.drawCircle(c.cx, c.cy, c.r, stroke);
        text.setColor(c.pressed ? Color.WHITE : withAlpha(labelColored ? color : Color.WHITE, 0xCC));
        cv.drawText(c.label, c.cx, c.cy - (text.descent() + text.ascent()) / 2f, text);
    }

    private void drawRect(Canvas cv, Ctl c) {
        float rad = 10 * dp;
        fill.setColor(c.pressed ? 0x99FFFFFF : 0x30000000);
        cv.drawRoundRect(c.rect, rad, rad, fill);
        stroke.setColor(c.pressed ? 0xFFFFFFFF : 0x99FFFFFF);
        cv.drawRoundRect(c.rect, rad, rad, stroke);
        text.setColor(c.pressed ? Color.BLACK : 0xCCFFFFFF);
        cv.drawText(c.label, c.rect.centerX(), c.rect.centerY() - (text.descent() + text.ascent()) / 2f, text);
    }

    private void drawDpad(Canvas cv) {
        float r = dpad.r, w = r * 0.38f;
        String[] arrows = { "▲", "▼", "◀", "▶" };
        float[][] off = { { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } };
        for (int i = 0; i < 4; i++) {
            boolean on = (dpadMask & (1 << i)) != 0;
            float cx = dpad.cx + off[i][0] * r * 0.62f, cy = dpad.cy + off[i][1] * r * 0.62f;
            RectF rc = new RectF(cx - w, cy - w, cx + w, cy + w);
            fill.setColor(on ? 0x99FFFFFF : 0x30000000);
            cv.drawRoundRect(rc, 6 * dp, 6 * dp, fill);
            stroke.setColor(on ? 0xFFFFFFFF : 0x99FFFFFF);
            cv.drawRoundRect(rc, 6 * dp, 6 * dp, stroke);
            text.setColor(on ? Color.BLACK : 0xCCFFFFFF);
            cv.drawText(arrows[i], cx, cy - (text.descent() + text.ascent()) / 2f, text);
        }
    }

    private void drawStick(Canvas cv, Stick st) {
        boolean active = st.pointer != -1;
        if (!active) return;   // the sticks appear only under a finger
        fill.setColor(active ? 0x40FFFFFF : 0x25000000);
        cv.drawCircle(st.baseX, st.baseY, st.radius, fill);
        stroke.setColor(active ? 0xCCFFFFFF : 0x77FFFFFF);
        cv.drawCircle(st.baseX, st.baseY, st.radius, stroke);
        float kx = st.knobX, ky = st.knobY;
        float dx = kx - st.baseX, dy = ky - st.baseY, len = (float) Math.hypot(dx, dy);
        if (len > st.radius) { kx = st.baseX + dx / len * st.radius; ky = st.baseY + dy / len * st.radius; }
        fill.setColor(st.clicked ? 0xDDFFFFFF : (active ? 0xAAFFFFFF : 0x66FFFFFF));
        cv.drawCircle(kx, ky, st.radius * 0.45f, fill);
    }

    private static int withAlpha(int color, int alpha) { return (color & 0x00FFFFFF) | (alpha << 24); }
}
