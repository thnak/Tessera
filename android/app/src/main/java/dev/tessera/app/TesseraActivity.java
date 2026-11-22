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
    protected void onResume() {
        super.onResume();
        hideSystemBars();
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

    @Override
    protected void onPause() {
        if (hingeReader != null) {
            hingeReader.stop();
        }
        super.onPause();
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
