// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Insets;
import android.graphics.Rect;
import android.os.Build;
import android.text.InputType;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.accessibility.AccessibilityNodeProvider;
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
    /**
     * Null until the surface exists, because the provider needs the native shell to query. The
     * platform asks for it from the first accessibility traversal, which is after {@code
     * surfaceCreated}.
     */
    private TesseraAccessibilityProvider accessibilityProvider;
    /**
     * Runs once the native shell exists, so a metric the activity read before the surface was ready
     * is not lost.
     *
     * <p>This exists because of a real ordering trap: {@code onResume} runs BEFORE {@code
     * surfaceCreated}, so a fold read there has no native handle to push to. Without this, a device
     * that is already folded when the app starts reports no fold until the next configuration
     * change, which may never come.
     */
    private Runnable onNativeReady;

    public TesseraView(Context context) {
        super(context);
        getHolder().addCallback(this);
        setFocusable(true);
        setFocusableInTouchMode(true);
        // The engine draws every pixel, so the view must not add its own background.
        setBackground(null);
        // A view that draws its own controls advertises accessibility while keeping its semantics
        // out of the view hierarchy. Without this the platform sees one unlabelled rectangle.
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_YES);
        // The clipboard cache and its change listener. Installed here rather than with the surface
        // because a paste can arrive before the first frame, and the engine's seam is synchronous.
        final ClipboardManager clipboard =
                (ClipboardManager) context.getSystemService(Context.CLIPBOARD_SERVICE);
        if (clipboard != null) {
            clipboard.addPrimaryClipChangedListener(this::refreshClipboard);
            refreshClipboard();
        }
    }

    /** Called by the activity when it is being destroyed. */
    void destroy() {
        if (nativeHandle != 0) {
            nativeDestroy(nativeHandle);
            nativeHandle = 0;
        }
        accessibilityProvider = null;
    }

    /**
     * The native shell handle, or 0 before the surface exists.
     *
     * <p>The activity needs it to push metrics the view does not own — fold posture arrives from a
     * library the activity drives, not from a callback on this view. A zero handle means the engine
     * has nothing to receive it yet, and every native entry point treats that as a no-op.
     */
    long nativeHandle() {
        return nativeHandle;
    }

    /**
     * Registers a one-shot action to run as soon as the native shell exists, or immediately when it
     * already does. Used for metrics the activity reads before {@code surfaceCreated}.
     */
    void whenNativeReady(Runnable action) {
        if (action == null) {
            return;
        }
        if (nativeHandle != 0) {
            action.run();
            return;
        }
        onNativeReady = action;
    }

    // --- Surface lifecycle -------------------------------------------------------------------

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        if (nativeHandle == 0) {
            nativeHandle = nativeCreate(this, holder.getSurface());
            // The shell exists now, so the clipboard cache can be installed. Doing it here rather
            // than in the constructor is what gives the bridge a live handle.
            nativeClipboardAttach(nativeHandle, this);
        }
        nativeSurfaceCreated(nativeHandle, holder.getSurface());
        requestApplyInsets();
        // The shell exists now, so anything the activity read before this point can be pushed.
        if (onNativeReady != null) {
            onNativeReady.run();
        }
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

    /**
     * Forwards a key event to the engine, which is the only way a hardware Backspace reaches the
     * text field.
     *
     * <p>A {@link SurfaceView} receives key events only when it is focused, and the engine draws its
     * own text field, so the platform cannot route a key to "the field" — it routes it to this view.
     * The engine's edit controller then decides whether the key means anything (Backspace deletes,
     * an arrow moves the caret, a letter is ignored here because the IME commits it as text).
     *
     * <p>The system Back key is deliberately left to the platform: the engine models it as a
     * {@code BackRequested} event that needs a route stack, and the shell has none, so consuming it
     * here would break the navigation gesture.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // The platform's Copy/Cut/Paste keys are not logical keys the engine has a code for: they
        // are commands, and a phone's IME toolbar sends them as key events. Handling them here is
        // what makes the toolbar work; `translate_key_sample` would map them to "unknown" and drop
        // them.
        if (nativeHandle != 0 && event.getAction() == KeyEvent.ACTION_DOWN) {
            switch (event.getKeyCode()) {
                case KeyEvent.KEYCODE_COPY:
                    return nativeClipboardCopy(nativeHandle);
                case KeyEvent.KEYCODE_CUT:
                    return nativeClipboardCut(nativeHandle);
                case KeyEvent.KEYCODE_PASTE:
                    return nativeClipboardPaste(nativeHandle);
                default:
                    break;
            }
        }
        if (nativeHandle != 0 && event.getKeyCode() != KeyEvent.KEYCODE_BACK) {
            if (nativeKey(nativeHandle, event.getKeyCode(), event.getMetaState(), event.getAction(),
                    event.getRepeatCount(), event.getEventTime())) {
                return true;
            }
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (nativeHandle == 0) {
            return super.onTouchEvent(event);
        }
        // A tap is also how the platform learns the view wants the keyboard: a custom-rendered text
        // field cannot focus itself the way an EditText does, so the show request is explicit.
        //
        // The engine answers whether the tap landed on the field. Without that answer the keyboard
        // appeared on every tap anywhere in the window, which is what a plain scene should not do.
        if (event.getActionMasked() == MotionEvent.ACTION_UP) {
            final float density = getResources().getDisplayMetrics().density;
            final boolean onField =
                    nativeTapOnField(nativeHandle, event.getX() / density, event.getY() / density);
            if (onField) {
                requestFocus();
                InputMethodManager imm = (InputMethodManager) getContext().getSystemService(
                        Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
                }
            } else {
                // A tap elsewhere dismisses the keyboard, which is what every app does and what
                // makes the button below the field usable without a second gesture.
                InputMethodManager imm = (InputMethodManager) getContext().getSystemService(
                        Context.INPUT_METHOD_SERVICE);
                if (imm != null) {
                    imm.hideSoftInputFromWindow(getWindowToken(), 0);
                }
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

    // --- Accessibility -----------------------------------------------------------------------

    /**
     * Returns the provider that exposes the engine's semantics tree to a screen reader.
     *
     * <p>This is the whole reason a custom-drawn app is not a black box to TalkBack: the platform
     * only ever looks for virtual children here. It returns null before the surface exists, because
     * the provider asks the native shell for every answer and there is nothing to ask yet.
     *
     * <p>Every {@code AccessibilityNodeInfo} the provider hands out is owned by the caller, which is
     * the platform's contract.
     */
    @Override
    public AccessibilityNodeProvider getAccessibilityNodeProvider() {
        if (nativeHandle == 0) {
            return null;
        }
        if (accessibilityProvider == null) {
            accessibilityProvider = new TesseraAccessibilityProvider(this, nativeHandle);
        }
        return accessibilityProvider;
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

    /**
     * Pushes the fold posture and hinge to the engine.
     *
     * <p>The posture and axis are the engine's own enum values (0 none, 1 flat, 2 half-opened; axis
     * 0 none, 1 horizontal, 2 vertical) rather than the library's objects, so no AndroidX type
     * crosses the boundary. The hinge is in physical pixels, which is what the engine's decode rules
     * take; they convert to logical with the window's scale.
     */
    static native void nativeSetFold(long handle, int posture, int axis, int left, int top,
            int right, int bottom);

    /**
     * Pushes one hinge-sensor reading to the engine.
     *
     * <p>Separate from {@link #nativeSetFold} because it is a separate source at a separate rate:
     * the sensor fires while the user is mid-fold, when the posture has not changed yet. The value
     * is degrees with 0 fully closed and 180 fully flat, which is what the engine's decode rules
     * take; a negative value is the "no sensor" sentinel.
     */
    static native void nativeSetHingeAngle(long handle, float degrees);

    /**
     * The text around the caret, for the platform's {@code InputConnection} queries.
     *
     * <p>The engine owns the buffer, so the platform cannot read it: an IME that asks
     * {@code getTextBeforeCursor} and gets "" behaves as if the field were always empty, which
     * breaks suggestions, Backspace and the IME's own composing region. {@code before} and
     * {@code after} are UTF-16 code units, which is the unit the platform asks in.
     */
    static native String nativeTextAround(long handle, int before, int after);

    /** The selected text, or "" when the selection is collapsed. Used by cut/copy. */
    static native String nativeSelectedText(long handle);

    /**
     * Moves the caret or extends the selection, in UTF-16 units from the start of the buffer.
     *
     * <p>Without this the engine's caret and the platform's disagree after the user drags a
     * selection handle, and the next commit lands in the wrong place.
     */
    static native void nativeSetSelection(long handle, int start, int end);

    // --- Clipboard (docs/guides/android-lane.md section 3.16) --------------------------------

    /**
     * Installs the clipboard cache and the change listener.
     *
     * <p>{@code ClipboardManager} must be touched on a thread with a {@code Looper}, and the
     * engine's render thread has none. The cache is what lets the engine's synchronous
     * {@code hal::Clipboard} seam work without blocking on the UI thread.
     */
    static native void nativeClipboardAttach(long handle, TesseraView view);

    /** Refreshes the engine's cached clipboard after the platform reports a new clip. */
    static native void nativeClipboardChanged(String text);

    /** The text the view should write to the platform clipboard, as UTF-16. */
    static native String nativeClipboardText();

    /** Copies the selection to the clipboard. False when there is no selection. */
    static native boolean nativeClipboardCopy(long handle);

    /** Copies the selection to the clipboard and deletes it. False when there is no selection. */
    static native boolean nativeClipboardCut(long handle);

    /** Inserts the clipboard at the caret. False when the clipboard is empty or unavailable. */
    static native boolean nativeClipboardPaste(long handle);

    /** Selects the whole buffer. False when it is already fully selected. */
    static native boolean nativeSelectAll(long handle);

    /**
     * Posts the platform clipboard write to the UI thread.
     *
     * <p>Called from the engine's render thread, which has no {@code Looper}: a direct
     * {@code ClipboardManager} call from it throws.
     */
    void postClipboardWrite() {
        post(
                () -> {
                    final String text = nativeClipboardText();
                    final ClipboardManager manager =
                            (ClipboardManager) getContext().getSystemService(
                                    Context.CLIPBOARD_SERVICE);
                    if (manager != null) {
                        manager.setPrimaryClip(ClipData.newPlainText("Tessera", text));
                    }
                });
    }

    /** Reads the platform clipboard and hands it to the engine's cache. */
    private void refreshClipboard() {
        final ClipboardManager manager =
                (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (manager == null || !manager.hasPrimaryClip()) {
            nativeClipboardChanged("");
            return;
        }
        final ClipData clip = manager.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0) {
            nativeClipboardChanged("");
            return;
        }
        final CharSequence text = clip.getItemAt(0).coerceToText(getContext());
        nativeClipboardChanged(text != null ? text.toString() : "");
    }

    private static native boolean nativeOnTouch(long handle, MotionEvent event);

    /**
     * Whether a point, in LOGICAL pixels, is on the engine's text field.
     *
     * <p>The view asks this before showing the keyboard. A custom-rendered field cannot focus itself
     * the way an {@code EditText} does, so the platform needs the engine's answer; without it the
     * keyboard appears on every tap anywhere in the window.
     */
    private static native boolean nativeTapOnField(long handle, float x, float y);

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

    /**
     * Forwards one key event to the engine's edit controller.
     *
     * <p>A hardware key and an IME-synthesised key both arrive here as a {@link KeyEvent}: the shell
     * is a Java activity, so neither reaches the engine through the NDK's {@code AInputEvent} path.
     * Without this the engine never sees a Backspace, and the field cannot be emptied.
     *
     * <p>The keycode, meta state and action are the platform's own values; the native side maps them
     * with the same pure rules the NDK path uses. A key the engine has no logical code for returns
     * false, so the platform keeps its own handling (the system Back gesture, for one).
     *
     * @return true when the engine consumed the key.
     */
    static native boolean nativeKey(long handle, int keycode, int metaState, int action,
            int repeatCount, long eventTimeNanos);
}
