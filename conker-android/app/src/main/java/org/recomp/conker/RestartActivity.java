package org.recomp.conker;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

/** Runs in its own process (":restart"): waits for the game process to end,
 *  then starts the game again. Used when the title relaunches itself. */
public class RestartActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        final int pid = getIntent().getIntExtra("pid", -1);
        final Handler h = new Handler(Looper.getMainLooper());
        final long t0 = System.currentTimeMillis();
        h.post(new Runnable() {
            @Override public void run() {
                boolean alive = pid > 0 && new java.io.File("/proc/" + pid).exists();
                if (alive && System.currentTimeMillis() - t0 < 5000) { h.postDelayed(this, 100); return; }
                Intent i = new Intent(RestartActivity.this, ConkerActivity.class);
                i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(i);
                finish();
                android.os.Process.killProcess(android.os.Process.myPid());
            }
        });
    }
}
