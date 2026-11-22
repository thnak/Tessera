// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.app.Activity;
import android.graphics.Rect;

import androidx.window.layout.FoldingFeature;
import androidx.window.layout.WindowInfoTracker;
import androidx.window.layout.WindowLayoutInfo;

import java.util.List;

/**
 * Reads fold posture and hinge bounds and hands them to the engine (issue #189,
 * docs/design/mobile-integration.md section 3.5).
 *
 * <p><b>Why this needs a library.</b> The platform SDK has no fold API at all: {@code android.jar}
 * for API 35, 36.1 and 37 contains no class matching fold, hinge or posture, and
 * {@code android.view.WindowInfo} does not exist at any of those levels. Posture is exposed only by
 * Jetpack WindowManager ({@code androidx.window:window}), which is Google's official and
 * Apache-2.0-licensed answer for exactly this. {@code DisplayShape} exists in the platform but
 * describes the outline of a display, not a hinge, so it cannot substitute.
 *
 * <p><b>Why there is a Kotlin helper.</b> The tracker's only continuous API is a Kotlin {@code
 * Flow}. Collecting one from Java means calling suspend functions through {@code BuildersKt} with a
 * hand-written {@code Function2}, which is fragile; {@link FoldObserver} does it in three lines of
 * Kotlin instead. The mapping stays here, in Java, where the rest of the shell is.
 *
 * <p><b>Why a listener is needed at all.</b> A fold change is not always a configuration change: the
 * emulator's {@code HALF_OPENED} transition changes the hinge angle without resizing the window, so
 * {@code onConfigurationChanged} never fires. A re-read on that hook alone would miss it.
 */
final class TesseraFoldReader {
    /** Matches {@code hal::PhysicalFoldPosture}. */
    private static final int POSTURE_NONE = 0;
    private static final int POSTURE_FLAT = 1;
    private static final int POSTURE_HALF_OPENED = 2;

    /** Matches {@code hal::PhysicalFoldAxis}. */
    private static final int AXIS_NONE = 0;
    private static final int AXIS_HORIZONTAL = 1;
    private static final int AXIS_VERTICAL = 2;

    private final Activity activity;
    private FoldObserver observer;

    TesseraFoldReader(Activity activity) {
        this.activity = activity;
    }

    /**
     * Starts observing fold changes until {@link #stop()}.
     *
     * <p>Called once the native shell exists, because a push before that has no handle to reach. The
     * observer's first emission is the current layout, so subscribing is also the initial read.
     *
     * <p>A failure to observe is not fatal: the synchronous read still runs on every configuration
     * change, so the app degrades to "fold updates on resize" rather than losing the feature.
     */
    void start(long handle) {
        if (handle == 0 || observer != null) {
            return;
        }
        try {
            observer = new FoldObserver(activity);
            observer.start(info -> pushLayout(handle, info));
        } catch (RuntimeException | LinkageError error) {
            // No coroutines on this build, or the library cannot answer: fall back to the
            // synchronous read, which still runs on every configuration change.
            observer = null;
            push(handle);
        }
    }

    /** Stops observing. Safe twice, and safe when {@link #start} never ran. */
    void stop() {
        if (observer != null) {
            observer.stop();
            observer = null;
        }
    }

    /**
     * Reads the current fold once and pushes it.
     *
     * <p>The fallback path when the observer could not start, and the path a configuration change
     * takes. A device with no fold, or a library that cannot answer, reports {@code POSTURE_NONE},
     * which is the same answer a non-foldable gives: the engine applies no hinge inset and lays out
     * as one rectangle.
     */
    void push(long handle) {
        if (handle == 0) {
            return;
        }
        WindowLayoutInfo info = null;
        try {
            info = WindowInfoTracker.getOrCreate(activity).getCurrentWindowLayoutInfo(activity);
        } catch (RuntimeException | LinkageError error) {
            // A library that cannot answer must not take the app down: no fold is a truthful answer.
            info = null;
        }
        pushLayout(handle, info);
    }

    /**
     * Reduces one layout to the engine's vocabulary and pushes it.
     *
     * <p>Shared by the observer and the synchronous read so the two cannot disagree about which
     * feature wins or how a posture maps.
     */
    private static void pushLayout(long handle, WindowLayoutInfo info) {
        if (handle == 0) {
            return;
        }
        int posture = POSTURE_NONE;
        int axis = AXIS_NONE;
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        final List<FoldingFeature> folds = info == null ? null : info.getDisplayFeatures()
                .stream()
                .filter(FoldingFeature.class::isInstance)
                .map(FoldingFeature.class::cast)
                .toList();
        if (folds != null && !folds.isEmpty()) {
            // A device reports at most one fold today. If a future device reports several, the first
            // is the one that splits the window; picking it deterministically beats summing hinges
            // into a rectangle that does not exist.
            final FoldingFeature fold = folds.get(0);
            posture = postureOf(fold);
            axis = axisOf(fold);
            final Rect bounds = fold.getBounds();
            left = bounds.left;
            top = bounds.top;
            right = bounds.right;
            bottom = bounds.bottom;
        }
        TesseraView.nativeSetFold(handle, posture, axis, left, top, right, bottom);
    }

    /**
     * The engine posture for one feature.
     *
     * <p>{@code FLAT} means the device is unfolded and the two halves are coplanar: a seam, not a
     * barrier, so the engine reports it as a flat fold with no occluding hinge. {@code HALF_OPENED}
     * is the posture that actually occludes, and only then is the hinge rectangle meaningful.
     */
    private static int postureOf(FoldingFeature fold) {
        final FoldingFeature.State state = fold.getState();
        if (FoldingFeature.State.FLAT.equals(state)) {
            return POSTURE_FLAT;
        }
        if (FoldingFeature.State.HALF_OPENED.equals(state)) {
            return POSTURE_HALF_OPENED;
        }
        return POSTURE_NONE;
    }

    /**
     * The engine axis for one feature.
     *
     * <p>The library's {@code Orientation} describes the axis the fold runs along, which is the same
     * sense the engine's {@code FoldAxis} uses: a {@code VERTICAL} fold is a vertical seam, so it
     * splits the window into left and right.
     */
    private static int axisOf(FoldingFeature fold) {
        final FoldingFeature.Orientation orientation = fold.getOrientation();
        if (FoldingFeature.Orientation.VERTICAL.equals(orientation)) {
            return AXIS_VERTICAL;
        }
        if (FoldingFeature.Orientation.HORIZONTAL.equals(orientation)) {
            return AXIS_HORIZONTAL;
        }
        return AXIS_NONE;
    }
}
