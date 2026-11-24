// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.os.Bundle;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/**
 * The Android app shell's activity (issue #189, docs/design/mobile-integration.md sections 3.5 and
 * 3.6).
 *
 * <p>Why this exists instead of {@code android.app.NativeActivity}: the platform reports window
 * insets, display cutouts and fold posture through Java APIs ({@code WindowInsets},
 * {@code DisplayCutout}, {@code FoldingFeature}) that have no NDK equivalent. A NativeActivity can
 * hand the engine an {@code ANativeWindow} but cannot read those, so the shell needs one Java class
 * to read them and pass them across JNI.
 *
 * <p>The activity owns no rendering. It creates the {@link TesseraView}, which owns the surface and
 * forwards input, and it reports metrics changes to the native side. Everything else — the scene,
 * layout, paint, the frame loop — stays in C++.
 */
public final class TesseraActivity extends android.app.Activity {
    static {
        System.loadLibrary("tessera_android_app");
    }

    private TesseraView view;
    private TesseraFoldReader foldReader;
    private TesseraHingeReader hingeReader;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        view = new TesseraView(this);
        setContentView(view);
        // Edge-to-edge: the engine draws the whole window and applies the insets itself, so the
        // system must not also inset the content. This is what makes the status/navigation bar
        // insets meaningful to the scene instead of being consumed by the framework.
        hideSystemBars();
        foldReader = new TesseraFoldReader(this);
        hingeReader = new TesseraHingeReader(this);
    }

    @Override
    protected void onStop() {
        // Stop rendering before the app leaves the screen. `surfaceDestroyed` does NOT fire for a
        // backgrounded app on current Android, so without this the render thread keeps presenting
        // into a buffer queue the compositor stopped consuming: the queue wedges, the task's
        // snapshot never completes, and returning shows a starting window over an app that has no
        // input focus and ANRs on every touch.
        if (view != null) {
            view.pauseRendering();
        }
        super.onStop();
    }

    @Override
    protected void onStart() {
        super.onStart();
    }

    @Override
    protected void onPause() {
        // Stop rendering here, not in `onStop`: an app can sit paused indefinitely without ever
        // being stopped (the emulator's task snapshot keeps the window "visible" for the purposes
        // of the lifecycle), and a render thread left presenting into a queue the compositor no
        // longer consumes is what wedges the task and ANRs it on return. Pausing early is the only
        // hook that is guaranteed to run.
        if (view != null) {
            view.pauseRendering();
        }
        // Both readers stop on pause: the hinge sensor is a non-wake-up sensor the platform expects
        // to be released, and the fold observer's coroutine would otherwise keep collecting for a
        // window no frame can show. `onResume` starts both again.
        if (foldReader != null) {
            foldReader.stop();
        }
        if (hingeReader != null) {
            hingeReader.stop();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
        // Rendering was paused in `onPause`; this is its matching resume. `start_host` is
        // idempotent, so a first start (where `surfaceCreated` has not run yet) is a no-op and the
        // surface callback starts it.
        if (view != null) {
            view.resumeRendering();
        }
        // The view must take focus back after the app returns from the background. Without this the
        // window has no focused view, and the platform reports "Application does not have a focused
        // window" for every touch: the app looks alive but nothing responds, and the input
        // dispatcher eventually raises an ANR.
        //
        // `requestFocusFromTouch` rather than `requestFocus`: the platform tracks a touch-mode flag
        // per window, and a view that only took focus in non-touch mode does not receive touches
        // after the window comes back. The ANR trace names exactly that
        // (`TouchModeEvent(inTouchMode=false)`).
        if (view != null) {
            view.requestFocusFromTouch();
        }
        // The first read happens here rather than in onCreate: the window layout is not final until
        // the activity is resumed, and a fold read before that reports the pre-layout bounds.
        pushFold();
        startHinge();
    }

    /**
     * Starts the hinge sensor once the native shell exists.
     *
     * <p>The sensor is registered on resume and released on pause, which is the platform's rule for
     * a non-wake-up sensor: leaving it registered while the app is backgrounded would keep the
     * sensor powered for a value no frame can show.
     */
    private void startHinge() {
        if (hingeReader == null || view == null) {
            return;
        }
        view.whenNativeReady(() -> hingeReader.start(view.nativeHandle()));
    }

    /**
     * Re-reads the fold after a configuration change.
     *
     * <p>Unfolding a device is a configuration change (the window bounds change with it), so this is
     * the hook that keeps the engine's fold state current. The manifest declares the changes this
     * activity handles itself, so the platform does not recreate it and the scene keeps its state.
     */
    @Override
    public void onConfigurationChanged(android.content.res.Configuration configuration) {
        super.onConfigurationChanged(configuration);
        pushFold();
    }

    /**
     * Takes focus back when the window regains it.
     *
     * <p>Returning from the background can leave the window without a focused view even though
     * {@code onResume} ran, and the platform then refuses to dispatch input: every touch is dropped
     * and the dispatcher raises an ANR. Asking here as well as in {@code onResume} covers both
     * orderings.
     */
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && view != null && !view.hasFocus()) {
            view.requestFocusFromTouch();
        }
    }

    /** Pushes the current fold posture and hinge to the engine, if the surface exists yet. */
    private void pushFold() {
        if (foldReader == null || view == null) {
            return;
        }
        // `onResume` runs before `surfaceCreated`, so the first read usually has no native handle.
        // Registering it means the fold is applied the moment the shell exists instead of being
        // dropped until the next configuration change.
        view.whenNativeReady(() -> foldReader.start(view.nativeHandle()));
    }

    /**
     * Hides the system bars and asks for edge-to-edge layout. The engine receives the resulting
     * insets through {@link TesseraView} and applies them as safe-area padding.
     */
    private void hideSystemBars() {
        WindowInsetsController controller = getWindow().getInsetsController();
        if (controller == null) {
            return;
        }
        controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
        controller.setSystemBarsBehavior(
                WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
    }

    @Override
    protected void onDestroy() {
        if (foldReader != null) {
            foldReader.stop();
        }
        if (hingeReader != null) {
            hingeReader.stop();
        }
        if (view != null) {
            view.destroy();
            view = null;
        }
        foldReader = null;
        hingeReader = null;
        super.onDestroy();
    }
}
