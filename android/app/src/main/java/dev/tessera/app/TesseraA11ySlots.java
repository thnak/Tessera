// SPDX-License-Identifier: MIT

package dev.tessera.app;

/**
 * The packed {@code long[]} layout one {@code nativeDescribe} call fills, and the arithmetic over it.
 *
 * <p><b>Why this is its own class.</b> Every index here is derived from {@link #FLAG_COUNT}, and the
 * Java side and {@code a11y_bridge.cpp} must agree on the layout or a screen reader reads the wrong
 * slot: a node's width arrives where its action count was expected. That is the kind of bug this
 * class exists to make testable, and it cannot be tested through {@code AccessibilityNodeInfo}
 * because reading a node needs an Android runtime. As arithmetic on constants it needs nothing, so
 * {@code TesseraA11ySlotsTest} pins it in a plain JUnit run - which is the only test infrastructure
 * available here (no Robolectric, and none can be fetched offline).
 *
 * <p>The layout is, in order: flags, bounds in 1/64 px, the collection position, action count + ids,
 * child count + ids. {@code a11y_bridge.cpp} writes it with the same names; a change to one without
 * the other is what the test in {@code tests/unit/hal/android_a11y_tests.cpp} catches on the C++ side.
 */
final class TesseraA11ySlots {
    private TesseraA11ySlots() {}

    static final int FLAG_ENABLED = 0;
    static final int FLAG_FOCUSABLE = 1;
    static final int FLAG_CHECKABLE = 2;
    static final int FLAG_CHECKED = 3;
    static final int FLAG_SELECTED = 4;
    static final int FLAG_EDITABLE = 5;
    static final int FLAG_SCROLLABLE = 6;
    static final int FLAG_ACCESSIBILITY_FOCUSED = 7;
    /** Derived: a node that offers Press is clickable. TalkBack gates activation on this flag. */
    static final int FLAG_CLICKABLE = 8;
    static final int FLAG_LONG_CLICKABLE = 9;
    /** The node is one item of a collection, and the two slots below describe where it sits. */
    static final int FLAG_COLLECTION_ITEM = 10;
    static final int FLAG_COUNT = 11;

    // Bounds follow the flags: x, y, width, height. Fixed point in 1/64 px, so a fractional logical
    // position survives the crossing and rounds once, here, instead of being truncated twice.
    static final int BOUNDS_X = FLAG_COUNT;
    static final int BOUNDS_Y = FLAG_COUNT + 1;
    static final int BOUNDS_W = FLAG_COUNT + 2;
    static final int BOUNDS_H = FLAG_COUNT + 3;
    /** 1-based position and the collection's item count, per {@code aria-posinset}/{@code aria-setsize}. */
    static final int POS_IN_SET_SLOT = FLAG_COUNT + 4;
    static final int SET_SIZE_SLOT = FLAG_COUNT + 5;

    static int actionCountIndex() {
        return FLAG_COUNT + 6;
    }

    static int actionIndex(int i) {
        return actionCountIndex() + 1 + i;
    }

    static int childCountIndex() {
        return actionCountIndex() + 1 + MAX_ACTIONS;
    }

    static int childIndex(int i) {
        return childCountIndex() + 1 + i;
    }

    /** The packed array's size: flags, bounds, collection position, action count + ids, child count + ids. */
    static int slotCount() {
        return childCountIndex() + 1 + MAX_CHILDREN;
    }

    /** Fixed-point 1/64 px back to a pixel coordinate, rounding half away from zero. */
    static int unpack(long value) {
        return (int) Math.round(value / 64.0);
    }

    /**
     * Whether a platform action carries a payload this class forwards.
     *
     * <p>{@code ACTION_SET_TEXT} carries the new text; {@code ACTION_SET_SELECTION} carries a
     * start/end pair, which is <b>not</b> a string and must not be squeezed into one.
     */
    static boolean carriesTextPayload(int action, int setTextAction) {
        return action == setTextAction;
    }

    /** The most action ids one node may offer. Mirrors {@code hal::android::kMaxActions}. */
    static final int MAX_ACTIONS = 8;
    /** The most children one snapshot entry may name. Mirrors {@code ui::a11y::kMaxChildren}. */
    static final int MAX_CHILDREN = 64;
}
