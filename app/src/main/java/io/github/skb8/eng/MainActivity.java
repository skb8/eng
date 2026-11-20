package io.github.skb8.eng;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("eng");
    }

    private long surfaceOwner;
    private int pointer = MotionEvent.INVALID_POINTER_ID;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        SurfaceView view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        view.setOnTouchListener(this::forwardTouch);
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
        // surfaceChanged supplies the window size for the first frame.
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        surfaceOwner = nativeSurface(holder.getSurface(), surfaceOwner, width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        if (surfaceOwner != 0) {
            nativeSurface(null, surfaceOwner, 0, 0);
            surfaceOwner = 0;
        }
        pointer = MotionEvent.INVALID_POINTER_ID;
    }

    private boolean forwardTouch(View view, MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            pointer = event.getPointerId(0);
        } else if (action == MotionEvent.ACTION_POINTER_UP
                && event.getPointerId(event.getActionIndex()) == pointer) {
            action = MotionEvent.ACTION_UP;
        }

        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_MOVE) {
            int index = event.findPointerIndex(pointer);
            if (index >= 0 && view.getWidth() > 0 && view.getHeight() > 0) {
                nativeTouch(surfaceOwner, action, event.getX(index) / view.getWidth(),
                        event.getY(index) / view.getHeight());
            }
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            nativeTouch(surfaceOwner, action, 0, 0);
            pointer = MotionEvent.INVALID_POINTER_ID;
        }
        return true;
    }

    private static native long nativeSurface(Surface surface, long owner, int width, int height);
    private static native void nativeTouch(long owner, int action, float x, float y);
}
