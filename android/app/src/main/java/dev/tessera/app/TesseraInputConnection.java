// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;


import android.view.inputmethod.InputConnectionWrapper;

/**
 * The engine's IME bridge (issue #189, docs/design/mobile-integration.md section 3.3).
 *
 * <p>A custom-rendered text field does not get an IME for free: the platform talks to an
 * {@link InputConnection}, and the engine's edit controller consumes {@code ui::InputEvent}. This
 * class is the adapter between the two, and it holds no text of its own — the engine owns the buffer,
 * the undo history and the selection, so a copy here would be a second source of truth that drifts.
 *
 * <p>Only the callbacks the engine models are overridden. Everything else keeps
 * {@link BaseInputConnection}'s behaviour, which is deliberately conservative: a callback this class
 * does not understand returns {@code false} so the platform does not believe text was inserted.
 *
 * <p>The action numbers match the engine's {@code ImeAction} (0 compose, 1 commit, 2 finish,
 * 3 cancel, 4 delete). They are a plain {@code int} across the boundary so the Java side does not
 * have to mirror a C++ enum.
 */
final class TesseraInputConnection extends InputConnectionWrapper {
    private static final int kActionCompose = 0;
    private static final int kActionCommit = 1;
    private static final int kActionFinish = 2;
    private static final int kActionCancel = 3;
    private static final int kActionDelete = 4;

    /** -1 means "leave the caret alone", matching the engine's sample default. */
    private static final int kNoCursor = -1;

    private final long handle;

    TesseraInputConnection(TesseraView view, long handle) {
        // The base connection needs a target for the calls this class does not override. The view is
        // that target, and `false` below the mutating methods keeps the platform from believing a
        // change happened when the engine refused it.
        super(new BaseInputConnection(view, false), false);
        this.handle = handle;
    }

    @Override
    public boolean setComposingText(CharSequence text, int newCursorPosition) {
        // The caret is relative to the end of the composing text in this callback, which is the same
        // convention the engine expects, so it is passed through unchanged.
        return TesseraView.nativeIme(handle, kActionCompose, text == null ? "" : text.toString(),
                newCursorPosition, 0, 0);
    }

    @Override
    public boolean finishComposingText() {
        return TesseraView.nativeIme(handle, kActionFinish, "", kNoCursor, 0, 0);
    }

    @Override
    public boolean commitText(CharSequence text, int newCursorPosition) {
        return TesseraView.nativeIme(handle, kActionCommit, text == null ? "" : text.toString(),
                newCursorPosition, 0, 0);
    }

    @Override
    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
        return TesseraView.nativeIme(handle, kActionDelete, "", kNoCursor, beforeLength,
                afterLength);
    }

    @Override
    public boolean sendKeyEvent(KeyEvent event) {
        // A hardware key arrives through the native input queue, so this path only exists for an IME
        // that synthesises one. The engine's key handling is the same either way.
        return super.sendKeyEvent(event);
    }

    @Override
    public boolean performEditorAction(int editorAction) {
        // The engine's edit controller has no editor-action concept yet (Done/Search/Go). Reporting
        // "not handled" lets the platform fall back instead of pretending it did something, and a
        // newline already arrives as a key event.
        return false;
    }
}
