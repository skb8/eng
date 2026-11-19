package io.github.skb8.eng;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.Toast;

public final class MainActivity extends Activity implements SurfaceHolder.Callback2 {
    static {
        System.loadLibrary("eng");
    }

    private long renderer;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        setContentView(view);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus) {
            return;
        }

        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        // The surface receives its initial size before surfaceRedrawNeeded.
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        releaseRenderer();
    }

    @Override
    public void surfaceRedrawNeeded(SurfaceHolder holder) {
        if (!holder.getSurface().isValid() || holder.getSurfaceFrame().isEmpty()) {
            return;
        }

        try {
            for (int attempt = 0; attempt < 2; ++attempt) {
                if (renderer == 0) {
                    renderer = nativeCreate(holder.getSurface());
                }
                if (nativeDraw(renderer)) {
                    return;
                }
                releaseRenderer();
            }
        } catch (RuntimeException error) {
            releaseRenderer();
            Log.e("eng", "Vulkan rendering failed", error);
            Toast.makeText(this, error.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        releaseRenderer();
    }

    private void releaseRenderer() {
        if (renderer != 0) {
            nativeDestroy(renderer);
            renderer = 0;
        }
    }

    private static native long nativeCreate(Surface surface);
    private static native boolean nativeDraw(long renderer);
    private static native void nativeDestroy(long renderer);
}
