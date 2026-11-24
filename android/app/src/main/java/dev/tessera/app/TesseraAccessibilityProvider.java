// SPDX-License-Identifier: MIT

package dev.tessera.app;

import android.graphics.Rect;
import android.os.Bundle;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityNodeProvider;

/**
 * The bridge that makes a custom-drawn window visible to TalkBack (issue #189,
 * docs/design/mobile-integration.md section 3.4).
 *
 * <p>A {@code SurfaceView} is one opaque rectangle to the accessibility framework: it has no child
 * views, so without this class a screen reader announces the whole app as a single empty view and
 * nothing inside it can be focused, read, or activated. {@link AccessibilityNodeProvider} is how a
 * view publishes virtual children it draws itself.
 *
 * <p><b>The shape is pull, not push.</b> The platform calls {@link #createAccessibilityNodeInfo}
 * synchronously, on the UI thread, and expects an answer before it returns. The engine renders on
 * its own thread and publishes snapshots; {@code nativeDescribe} reads the last published snapshot
 * under a short mutex. So the platform never waits for a frame, and a frame never waits for the
 * platform.
 *
 * <p><b>Virtual view ids are stable.</b> The native side keeps a scene node on the same id for as
 * long as it stays in the tree, so a focused control keeps its identity across a rebuild. Id
 * {@link #HOST_VIEW_ID} is the view itself, as the platform's contract requires.
 *
 * <p><b>This class holds no tree.</b> It asks the engine for one node at a time and copies the
 * answer into a platform {@link AccessibilityNodeInfo}. There is no second copy of the semantics to
 * keep in sync, which is the bug this shape avoids.
 *
 * <p><b>One native call per node.</b> Each query returns the whole description in one packed call
 * rather than one call per field: {@code createAccessibilityNodeInfo} runs on the UI thread while a
 * screen reader is moving, and eleven crossings where one will do is eleven chances to jank.
 */
final class TesseraAccessibilityProvider extends AccessibilityNodeProvider {
    /**
     * The platform's reserved id for the host view. It is {@code -1}, not {@code 0}: the platform
     * uses 0 for the view itself only in {@code AccessibilityNodeInfo.obtain(view)} and passes -1 to
     * a provider when it wants the host's own node. A real node is therefore any id {@code >= 1},
     * and the engine's provider also uses 0 for "no node".
     */
    static final int HOST_VIEW_ID = -1;

    /**
     * The packed {@code long[]} layout lives in {@link TesseraA11ySlots}, which is pure arithmetic so
     * a plain JUnit run can pin it. Every slot index below is a short alias for clarity at the use
     * sites; the values and the check that they agree with the native writer are in one place.
     */
    private static final int MAX_CHILDREN = TesseraA11ySlots.MAX_CHILDREN;
    private static final int MAX_ACTIONS = TesseraA11ySlots.MAX_ACTIONS;

    private static final int FLAG_ENABLED = TesseraA11ySlots.FLAG_ENABLED;
    private static final int FLAG_FOCUSABLE = TesseraA11ySlots.FLAG_FOCUSABLE;
    private static final int FLAG_CHECKABLE = TesseraA11ySlots.FLAG_CHECKABLE;
    private static final int FLAG_CHECKED = TesseraA11ySlots.FLAG_CHECKED;
    private static final int FLAG_SELECTED = TesseraA11ySlots.FLAG_SELECTED;
    private static final int FLAG_EDITABLE = TesseraA11ySlots.FLAG_EDITABLE;
    private static final int FLAG_SCROLLABLE = TesseraA11ySlots.FLAG_SCROLLABLE;
    private static final int FLAG_ACCESSIBILITY_FOCUSED = TesseraA11ySlots.FLAG_ACCESSIBILITY_FOCUSED;
    private static final int FLAG_CLICKABLE = TesseraA11ySlots.FLAG_CLICKABLE;
    private static final int FLAG_LONG_CLICKABLE = TesseraA11ySlots.FLAG_LONG_CLICKABLE;
    private static final int FLAG_COLLECTION_ITEM = TesseraA11ySlots.FLAG_COLLECTION_ITEM;
    private static final int BOUNDS_X = TesseraA11ySlots.BOUNDS_X;
    private static final int BOUNDS_Y = TesseraA11ySlots.BOUNDS_Y;
    private static final int BOUNDS_W = TesseraA11ySlots.BOUNDS_W;
    private static final int BOUNDS_H = TesseraA11ySlots.BOUNDS_H;
    private static final int POS_IN_SET_SLOT = TesseraA11ySlots.POS_IN_SET_SLOT;
    private static final int SET_SIZE_SLOT = TesseraA11ySlots.SET_SIZE_SLOT;
    private static final int SLOT_COUNT = TesseraA11ySlots.slotCount();

    private final TesseraView view;
    private final long nativeHandle;
    /** Reused by every query: {@code createAccessibilityNodeInfo} is called per node traversal. */
    private final long[] slots = new long[SLOT_COUNT];
    /** The class name, the node's text and its content description, as strings. */
    private final String[] text = new String[3];

    TesseraAccessibilityProvider(TesseraView view, long nativeHandle) {
        this.view = view;
        this.nativeHandle = nativeHandle;
    }

    /**
     * The engine's own "host" id: {@code ui::a11y::kHostVirtualId}. It is 0, while the platform's
     * is -1, so the two are deliberately different constants and this class translates between them
     * rather than making the engine adopt a platform detail.
     */
    private static final int ENGINE_HOST_ID = 0;

    @Override
    public AccessibilityNodeInfo createAccessibilityNodeInfo(int virtualViewId) {
        if (nativeHandle == 0) {
            return null;
        }
        if (virtualViewId == HOST_VIEW_ID) {
            return hostNode();
        }
        if (!nativeDescribe(nativeHandle, virtualViewId, text, slots)) {
            // A stale id from a tree the app already replaced is a normal race: the platform can
            // query an id it learned before a rebuild. Null makes it re-read the tree.
            return null;
        }
        final AccessibilityNodeInfo node = AccessibilityNodeInfo.obtain(view, virtualViewId);
        // The class name is what a screen reader announces as the control kind ("Button",
        // "EditText"). The engine maps its role onto the platform's own widget names.
        node.setClassName(text[0] != null ? text[0] : "android.view.View");
        node.setPackageName(view.getContext().getPackageName());
        // A virtual node is not part of the view hierarchy, so the platform does not know whether it
        // is on screen: without this flag BOTH TalkBack and `uiautomator dump` skip it entirely and
        // the window looks empty. It is the single most load-bearing line in this class.
        node.setVisibleToUser(true);
        if (text[1] != null && !text[1].isEmpty()) {
            node.setText(text[1]);
        }
        if (text[2] != null && !text[2].isEmpty()) {
            node.setContentDescription(text[2]);
        }
        node.setEnabled(slots[FLAG_ENABLED] != 0);
        node.setFocusable(slots[FLAG_FOCUSABLE] != 0);
        node.setCheckable(slots[FLAG_CHECKABLE] != 0);
        node.setChecked(slots[FLAG_CHECKED] != 0);
        node.setSelected(slots[FLAG_SELECTED] != 0);
        node.setEditable(slots[FLAG_EDITABLE] != 0);
        node.setScrollable(slots[FLAG_SCROLLABLE] != 0);
        // A collection item tells the platform where it sits, which is what makes "3 of 40" and
        // per-list navigation work (accessibility spec section 5). `rowIndex` and `columnIndex` are
        // -1 because the engine models a flat list, not a grid: the platform reads a flat item by
        // its position, and claiming a row would be an invented shape.
        if (slots[FLAG_COLLECTION_ITEM] != 0) {
            // The engine models a flat list, so the platform's row/column pair is
            // (position, one column) rather than an invented grid: `rowIndex` is the 0-based
            // position, which is `aria-posinset - 1`.
            final int setSize = (int) slots[SET_SIZE_SLOT];
            final int rowIndex = Math.max(0, (int) slots[POS_IN_SET_SLOT] - 1);
            node.setCollectionItemInfo(AccessibilityNodeInfo.CollectionItemInfo.obtain(
                    rowIndex,
                    1 /* rowSpan */,
                    0 /* columnIndex */,
                    1 /* columnSpan */,
                    false /* heading */,
                    false /* selected */));
            node.setCollectionInfo(AccessibilityNodeInfo.CollectionInfo.obtain(
                    setSize,
                    1 /* columnCount */,
                    false /* hierarchical */));
        }
        node.setAccessibilityFocused(slots[FLAG_ACCESSIBILITY_FOCUSED] != 0);
        // Clickable is not a separate engine concept: a node that offers Press is clickable, and
        // TalkBack decides whether a stop is actionable from this flag rather than from the action
        // list.
        node.setClickable(slots[FLAG_CLICKABLE] != 0);
        node.setLongClickable(slots[FLAG_LONG_CLICKABLE] != 0);

        // Bounds arrive in the engine's **logical** pixels, which are its layout units, while the
        // platform works in **physical** pixels. Converting here rather than in the engine keeps the
        // engine density-independent (the same layout at every density) and gives the platform the
        // space it expects. Getting this wrong puts every focus rectangle and every
        // touch-exploration hit test in the wrong place, which is worse than an empty tree: it
        // looks like it works and reads the wrong control.
        final float scale = view.getResources().getDisplayMetrics().density;
        final Rect rect = new Rect(Math.round(unpack(slots[BOUNDS_X]) * scale),
                Math.round(unpack(slots[BOUNDS_Y]) * scale),
                Math.round((unpack(slots[BOUNDS_X]) + unpack(slots[BOUNDS_W])) * scale),
                Math.round((unpack(slots[BOUNDS_Y]) + unpack(slots[BOUNDS_H])) * scale));
        node.setBoundsInParent(rect);
        // The window is edge-to-edge and fills the surface, so the parent rect is already in screen
        // space. A shell that letterboxes would add its offset here.
        node.setBoundsInScreen(rect);

        // Actions are the platform's own ids, supplied by the engine's mapping. Setting them is what
        // makes "double-tap to activate" work: without ACTION_CLICK a screen reader has no way to
        // press the control at all.
        final int actionCount = (int) slots[actionCountIndex()];
        for (int i = 0; i < actionCount && i < MAX_ACTIONS; ++i) {
            final int id = (int) slots[actionIndex(i)];
            if (id != 0) {
                node.addAction(id);
            }
        }

        final int childCount = (int) slots[childCountIndex()];
        for (int i = 0; i < childCount && i < MAX_CHILDREN; ++i) {
            node.addChild(view, (int) slots[childIndex(i)]);
        }
        return node;
    }

    /** The host view's own node: the window, which has no engine-side semantics of its own. */
    private AccessibilityNodeInfo hostNode() {
        final AccessibilityNodeInfo node = AccessibilityNodeInfo.obtain(view);
        node.setPackageName(view.getContext().getPackageName());
        node.setClassName("android.view.View");
        node.setSource(view);
        node.setVisibleToUser(true);
        final int rootCount = nativeChildren(nativeHandle, ENGINE_HOST_ID, slots);
        for (int i = 0; i < rootCount && i < MAX_CHILDREN; ++i) {
            // The host answers its children in the tail of `slots`, where a node's children live.
            node.addChild(view, (int) slots[childIndex(i)]);
        }
        return node;
    }

    /**
     * The text payload one platform action carries, or empty when it carries none.
     *
     * `ACTION_SET_TEXT` passes its `CharSequence` straight through: it is the one value-setting action
     * the engine models (`a11y::Action::SetValue`). `ACTION_SET_SELECTION` deliberately has no payload
     * here - the engine's `Action` has no selection member, so the bridge refuses that request in
     * `performAction` and lets the platform's default handling answer it, rather than accepting a
     * caret move it would never apply.
     */
    private static String payloadFor(int action, Bundle arguments) {
        if (arguments == null || action != AccessibilityNodeInfo.ACTION_SET_TEXT) {
            return "";
        }
        CharSequence text = arguments.getCharSequence(
                AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE);
        return text == null ? "" : text.toString();
    }

    @Override
    public boolean performAction(int virtualViewId, int action, Bundle arguments) {
        if (nativeHandle == 0) {
            return false;
        }
        // The action id is the platform's own and the engine maps it back to an engine action. The
        // engine queues the request; the window driver executes it on the next tick, on the thread
        // that owns the scene. True here means "accepted", not "already done", which is what the
        // platform expects from this call.
        //
        // The two value-setting actions carry a payload, and dropping it is why they used to be
        // offered and then ignored: `ACTION_SET_TEXT` brings the new text as a `CharSequence`, and
        // `ACTION_SET_SELECTION` brings a start/end pair. The engine's buffer is UTF-8 and its
        // selection is a UTF-8 byte offset, while both platform values are UTF-16 offsets, so the
        // selection is passed as text for the same reason a `SetText` is - the bridge cannot do the
        // conversion without the buffer, and the engine already owns one.
        // A selection request is a caret move, and the engine already owns one: the IME's own path
        // (`TesseraInputConnection`) sets it through `nativeSetSelection`, which converts UTF-16
        // offsets to the buffer's UTF-8 bytes and publishes the painted decoration. Routing here
        // through the same call means a screen reader's "select from here to here" and the user
        // dragging a handle end in exactly the same engine state - two implementations would be two
        // places to get the surrogate-pair arithmetic wrong.
        if (action == AccessibilityNodeInfo.ACTION_SET_SELECTION) {
            final int[] range = selectionRange(arguments);
            if (range == null) {
                // No usable start/end pair: fall back to the platform rather than moving the caret
                // somewhere arbitrary.
                return super.performAction(virtualViewId, action, arguments);
            }
            TesseraView.nativeSetSelection(nativeHandle, range[0], range[1]);
            return true;
        }
        final String payload = payloadFor(action, arguments);
        if (nativePerformAction(nativeHandle, virtualViewId, action, payload)) {
            return true;
        }
        return super.performAction(virtualViewId, action, arguments);
    }

    /**
     * The {@code (start, end)} UTF-16 range an {@code ACTION_SET_SELECTION} carries, or null.
     *
     * <p>The platform puts them under {@link AccessibilityNodeInfo#ACTION_ARGUMENT_SELECTION_START_INT}
     * and {@code ..._END_INT}. Either being absent is "no request" rather than "zero": a caller that
     * named only the start has asked for something this method will not invent an end for.
     */
    private static int[] selectionRange(Bundle arguments) {
        if (arguments == null) {
            return null;
        }
        if (!arguments.containsKey(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT)
                || !arguments.containsKey(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT)) {
            return null;
        }
        final int start =
                arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, -1);
        final int end = arguments.getInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, -1);
        if (start < 0 || end < 0) {
            return null;
        }
        return new int[] {start, end};
    }

    @Override
    public AccessibilityNodeInfo findFocus(int focus) {
        if (nativeHandle == 0) {
            return null;
        }
        if (focus != AccessibilityNodeInfo.FOCUS_ACCESSIBILITY
                && focus != AccessibilityNodeInfo.FOCUS_INPUT) {
            return null;
        }
        final int id = nativeFocusedId(nativeHandle);
        if (id == ENGINE_HOST_ID) {
            return null;
        }
        return createAccessibilityNodeInfo(id);
    }

    // --- Packed slot layout --------------------------------------------------------------------

    private static int actionCountIndex() {
        return TesseraA11ySlots.actionCountIndex();
    }

    private static int actionIndex(int i) {
        return TesseraA11ySlots.actionIndex(i);
    }

    private static int childCountIndex() {
        return TesseraA11ySlots.childCountIndex();
    }

    private static int childIndex(int i) {
        return TesseraA11ySlots.childIndex(i);
    }

    /** Fixed-point 1/64 px back to a pixel coordinate, rounding half away from zero. */
    private static int unpack(long value) {
        return TesseraA11ySlots.unpack(value);
    }

    // --- Native ------------------------------------------------------------------------------

    /**
     * Fills the caller's arrays with one node's whole description, in one crossing.
     *
     * @param text receives the class name, the node's text and its content description.
     * @param slots receives the packed flags, bounds, action ids and child ids.
     * @return true when the id is live, false when it is stale.
     */
    static native boolean nativeDescribe(long handle, int virtualViewId, String[] text,
            long[] slots);

    static native int nativeChildren(long handle, int virtualViewId, long[] slots);

    static native int nativeFocusedId(long handle);

    static native boolean nativePerformAction(long handle, int virtualViewId, int action,
            String text);
}
