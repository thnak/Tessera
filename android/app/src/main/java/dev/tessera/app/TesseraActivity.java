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

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        view = new TesseraView(this);
        setContentView(view);
        // Edge-to-edge: the engine draws the whole window and applies the insets itself, so the
        // system must not also inset the content. This is what makes the status/navigation bar
        // insets meaningful to the scene instead of being consumed by the framework.
        hideSystemBars();
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();
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
        if (view != null) {
            view.destroy();
            view = null;
        }
        super.onDestroy();
    }
}
