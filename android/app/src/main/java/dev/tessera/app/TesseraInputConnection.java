// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.view.KeyEvent;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
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
        // An IME that synthesises a key (Space, Enter, Backspace on a keyboard that sends key
        // events rather than committing text) reaches the engine only here: the event never goes
        // through the view's `dispatchKeyEvent`, because the IME owns the input connection. Without
        // this override the key is handed to the base connection, which has no engine behind it, so
        // Space and Enter silently do nothing while letters (committed as text) still work.
        //
        // The engine's key path is the same one a hardware key takes, so the mapping rules and the
        // edit controller's chord table are shared rather than duplicated.
        if (handle != 0 && TesseraView.nativeKey(handle, event.getKeyCode(), event.getMetaState(),
                event.getAction(), event.getRepeatCount(), event.getEventTime())) {
            return true;
        }
        return super.sendKeyEvent(event);
    }

    @Override
    public boolean performEditorAction(int editorAction) {
        // A multi-line field's Enter key arrives here, not as a key event: the IME reports an editor
        // action and expects the app to insert the line break itself. Reporting "not handled" made
        // Enter do nothing at all, which on a phone reads as a broken key.
        //
        // Only IME_ACTION_NONE (and the unspecified action) means "insert a newline"; the named
        // actions (Done/Search/Go/Send) are a form concern the engine has no concept of, so they
        // stay unhandled and the platform keeps its own behaviour.
        if (editorAction == EditorInfo.IME_ACTION_NONE || editorAction == EditorInfo.IME_ACTION_UNSPECIFIED) {
            if (handle != 0) {
                // The same key path a hardware Enter takes, so the field's multiline flag decides
                // whether the newline is inserted or gated off - one rule, not two.
                final long now = System.currentTimeMillis();
                final boolean down = TesseraView.nativeKey(handle, KeyEvent.KEYCODE_ENTER, 0,
                        KeyEvent.ACTION_DOWN, 0, now);
                final boolean up = TesseraView.nativeKey(handle, KeyEvent.KEYCODE_ENTER, 0,
                        KeyEvent.ACTION_UP, 0, now);
                if (down || up) {
                    return true;
                }
            }
        }
        return false;
    }

    // --- Text queries -------------------------------------------------------------------------
    //
    // The engine owns the buffer, so these must read it back rather than answer from a copy. An IME
    // that gets "" from getTextBeforeCursor treats the field as empty: it stops offering
    // suggestions, Backspace deletes nothing, and its composing region is placed at the wrong
    // offset. The platform asks in UTF-16 code units, which is what the native side converts.

    @Override
    public CharSequence getTextBeforeCursor(int length, int flags) {
        if (length <= 0) {
            return "";
        }
        return TesseraView.nativeTextAround(handle, length, 0);
    }

    @Override
    public CharSequence getTextAfterCursor(int length, int flags) {
        if (length <= 0) {
            return "";
        }
        return TesseraView.nativeTextAround(handle, 0, length);
    }

    @Override
    public CharSequence getSelectedText(int flags) {
        return TesseraView.nativeSelectedText(handle);
    }

    // --- Clipboard (docs/guides/android-lane.md section 3.16) --------------------------------
    //
    // The IME's own context menu (and the platform's Copy/Paste toolbar) calls these, not the
    // engine's key chords: a phone has no Ctrl key. The engine owns the buffer and the selection, so
    // these forward rather than answering from a copy.
    //
    // No `@Override`: `BaseInputConnection` does not implement these, so they are new methods on
    // this class rather than overrides. The platform reaches them through the `InputConnection`
    // interface, which is what the IME holds.

    public boolean copy() {
        return TesseraView.nativeClipboardCopy(handle);
    }

    public boolean cut() {
        return TesseraView.nativeClipboardCut(handle);
    }

    public boolean paste() {
        return TesseraView.nativeClipboardPaste(handle);
    }

    /**
     * Selects the whole buffer.
     *
     * <p>No {@code @Override}: {@code BaseInputConnection} does not implement it, so this is a new
     * method the platform reaches through the {@code InputConnection} interface.
     */
    public boolean selectAll() {
        return TesseraView.nativeSelectAll(handle);
    }

    @Override
    public boolean setSelection(int start, int end) {
        // A negative offset is the platform's "no selection"; the engine clamps, so passing it
        // through would move the caret to the start instead of leaving it alone.
        if (start < 0 || end < 0) {
            return false;
        }
        TesseraView.nativeSetSelection(handle, start, end);
        return true;
    }

    @Override
    public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
        // The engine's delete path is per code point already, so the code-point variant is the same
        // call as the UTF-16 one. Overriding it matters because the base class would otherwise
        // answer false and the IME would fall back to a slower path that also fails.
        return deleteSurroundingText(beforeLength, afterLength);
    }
}
