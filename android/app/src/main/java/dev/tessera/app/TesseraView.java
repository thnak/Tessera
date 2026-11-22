// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.content.Context;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.text.InputType;
import android.view.DisplayCutout;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;

import java.util.List;

/**
 * The surface and metrics bridge (issue #189, docs/design/mobile-integration.md sections 3.1, 3.5
 * and 3.6).
 *
 * <p>It is a {@link SurfaceView} because that is the one view whose {@link Surface} the NDK can turn
 * into an {@code ANativeWindow} ({@code ANativeWindow_fromSurface}). A {@code TextureView} would give
 * a {@code SurfaceTexture} instead, which needs a different Vulkan path.
 *
 * <p>It owns three responsibilities, each a thin forward to native code:
 *
 * <ul>
 *   <li><b>Surface lifecycle.</b> {@code surfaceCreated}/{@code surfaceDestroyed} tell the engine
 *       when it may render. The engine must never present into a destroyed surface.
 *   <li><b>Metrics.</b> Insets, cutouts and fold posture are read here and pushed across JNI. The
 *       decode rules live in C++ ({@code android_metrics_translation.cpp}) so they are unit-tested
 *       on every lane; this class only reads the platform values.
 *   <li><b>Input.</b> Touch events are forwarded to the engine's translator, which owns the
 *       normalization.
 * </ul>
 */
public final class TesseraView extends SurfaceView implements SurfaceHolder.Callback {
    private long nativeHandle;

    public TesseraView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        // The engine draws every pixel, so the view must not add its own background.
        setBackground(null);
    }

    /** Called by the activity when it is being destroyed. */
    void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
    }

    // --- Surface lifecycle -------------------------------------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (nativeHandle == 0) {
            nativeHandle = nativeCreate(this, holder.getSurface());
        }
        nativeSurfaceCreated(nativeHandle, holder.getSurface());
        requestApplyInsets();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeSurfaceChanged(nativeHandle, holder.getSurface(), width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        // The engine must stop rendering BEFORE the surface goes away: a present into a destroyed
        // surface is undefined behaviour on Android.
        nativeSurfaceDestroyed(nativeHandle);
    }

    // --- Metrics -----------------------------------------------------------------------------

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        if (nativeHandle != 0) {
            pushInsets(insets);
            pushCutouts(insets);
        }
        return super.onApplyWindowInsets(insets);
    }

    private void pushInsets(WindowInsets insets) {
        // The system bars and the IME are separate sources in the engine's metrics, so each is read
        // with its own type mask rather than summed.
        Insets bars =
                insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
        Insets ime = insets.getInsets(WindowInsets.Type.ime());
        Insets gestures = insets.getInsets(WindowInsets.Type.systemGestures());
        nativeSetInsets(nativeHandle, bars.left, bars.top, bars.right, bars.bottom, ime.left,
                ime.top, ime.right, ime.bottom, gestures.left, gestures.top, gestures.right,
                gestures.bottom);
    }

    private void pushCutouts(WindowInsets insets) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            return;
        }
        DisplayCutout cutout = insets.getDisplayCutout();
        if (cutout == null) {
            nativeSetCutouts(nativeHandle, new int[0]);
            return;
        }
        List<Rect> rects = cutout.getBoundingRects();
        int[] flat = new int[rects.size() * 4];
        for (int i = 0; i < rects.size(); ++i) {
            Rect rect = rects.get(i);
            flat[i * 4] = rect.left;
            flat[i * 4 + 1] = rect.top;
            flat[i * 4 + 2] = rect.right;
            flat[i * 4 + 3] = rect.bottom;
        }
        nativeSetCutouts(nativeHandle, flat);
    }

    // --- Input -------------------------------------------------------------------------------

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (nativeHandle == 0) {
            return super.onTouchEvent(event);
        }
        // A tap is also how the platform learns the view wants the keyboard: a custom-rendered text
        // field cannot focus itself the way an EditText does, so the show request is explicit.
        if (event.getActionMasked() == MotionEvent.ACTION_UP) {
            requestFocus();
            InputMethodManager imm = (InputMethodManager) getContext().getSystemService(
                    Context.INPUT_METHOD_SERVICE);
            if (imm != null) {
                imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
            }
        }
        // The engine's translator owns the normalization; this only forwards. Returning true keeps
        // the gesture stream coming, which a multi-touch sequence needs.
        return nativeOnTouch(nativeHandle, event);
    }

    // --- IME ---------------------------------------------------------------------------------

    /**
     * Tells the platform this view can host a text editor, which is what makes the soft keyboard
     * appear for it at all.
     *
     * <p>It returns true unconditionally because the whole window is one surface: the engine draws
     * its own text field and hit-tests the tap, so the platform cannot know which part of the window
     * is editable. The cost of the simplification is that the keyboard can be shown while no text
     * field is focused; the engine's edit controller simply ignores the events, which is the same
     * thing it does for any event outside a field.
     *
     * <p>When the engine grows a "focused text field" signal, this becomes a conditional and the
     * keyboard stops appearing on a plain scene.
     */
    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }

    /**
     * Hands the platform an {@link InputConnection} so the soft keyboard can talk to the engine.
     *
     * <p>A custom-rendered text field does not get an IME for free: the platform only starts one if
     * the focused view returns a connection here. The connection forwards each callback to the
     * engine's edit controller, which owns the text buffer and the undo history — this class never
     * holds text of its own.
     *
     * <p>{@link BaseInputConnection} is used rather than a bare {@link InputConnection} because it
     * implements the parts a custom view genuinely does not care about (the extracted-text getters,
     * the batch-edit protocol) while leaving the mutating calls to be overridden here.
     */
    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
        if (nativeHandle == 0) {
            return null;
        }
        // The engine draws its own text field, so the platform must not draw a second one; it only
        // needs to know what kind of content this is.
        outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return new TesseraInputConnection(this, nativeHandle);
    }

    // --- Native ------------------------------------------------------------------------------

    private static native long nativeCreate(TesseraView view, Surface surface);

    private static native void nativeDestroy(long handle);

    private static native void nativeSurfaceCreated(long handle, Surface surface);

    private static native void nativeSurfaceChanged(long handle, Surface surface, int width,
            int height);

    private static native void nativeSurfaceDestroyed(long handle);

    private static native void nativeSetInsets(long handle, int barsLeft, int barsTop,
            int barsRight, int barsBottom, int imeLeft, int imeTop, int imeRight, int imeBottom,
            int gestureLeft, int gestureTop, int gestureRight, int gestureBottom);

    private static native void nativeSetCutouts(long handle, int[] rects);

    private static native boolean nativeOnTouch(long handle, MotionEvent event);

    /**
     * Forwards one IME callback to the engine's edit controller.
     *
     * <p>The action is an index into the engine's {@code ImeAction}: 0 compose, 1 commit, 2 finish,
     * 3 cancel, 4 delete. The text crosses as a Java {@code String} because that is what the platform
     * hands us; the native side reads it into UTF-16 and converts to the engine's UTF-8 payload.
     *
     * @return true when the engine consumed the callback.
     */
    static native boolean nativeIme(long handle, int action, String text, int cursorUtf16,
            int deleteBefore, int deleteAfter);
}
