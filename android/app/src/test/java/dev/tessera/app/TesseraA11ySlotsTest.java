// SPDX-License-Identifier: MIT

package dev.tessera.app;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

/**
 * The packed slot layout the accessibility bridge fills (issue #889).
 *
 * <p><b>Why these assertions are worth having.</b> Every index in {@link TesseraA11ySlots} is derived
 * from {@link TesseraA11ySlots#FLAG_COUNT}, and {@code a11y_bridge.cpp} writes the array with the same
 * derivation. If the two drift, a screen reader reads a node's width where its action count was
 * expected - the node announces nonsense or nothing, and no crash points at the cause. The C++ side
 * has its own check ({@code tests/unit/hal/android_a11y_tests.cpp}); this is the Java half, and it is
 * a plain JUnit run because the layout is arithmetic, not Android.
 *
 * <p>The expected numbers are written out rather than recomputed from the same constants: a test that
 * derives its expectation the same way as the code under test passes when both are wrong.
 */
public class TesseraA11ySlotsTest {

    @Test
    public void flagsAreContiguousFromZero() {
        // The native side writes flag i into slot i, so the block must start at 0 and have no gap.
        assertEquals(0, TesseraA11ySlots.FLAG_ENABLED);
        assertEquals(1, TesseraA11ySlots.FLAG_FOCUSABLE);
        assertEquals(2, TesseraA11ySlots.FLAG_CHECKABLE);
        assertEquals(3, TesseraA11ySlots.FLAG_CHECKED);
        assertEquals(4, TesseraA11ySlots.FLAG_SELECTED);
        assertEquals(5, TesseraA11ySlots.FLAG_EDITABLE);
        assertEquals(6, TesseraA11ySlots.FLAG_SCROLLABLE);
        assertEquals(7, TesseraA11ySlots.FLAG_ACCESSIBILITY_FOCUSED);
        assertEquals(8, TesseraA11ySlots.FLAG_CLICKABLE);
        assertEquals(9, TesseraA11ySlots.FLAG_LONG_CLICKABLE);
        assertEquals(10, TesseraA11ySlots.FLAG_COLLECTION_ITEM);
        assertEquals(11, TesseraA11ySlots.FLAG_COUNT);
    }

    @Test
    public void boundsFollowTheFlags() {
        // Four slots, in the order the Java side reads them: x, y, width, height.
        assertEquals(11, TesseraA11ySlots.BOUNDS_X);
        assertEquals(12, TesseraA11ySlots.BOUNDS_Y);
        assertEquals(13, TesseraA11ySlots.BOUNDS_W);
        assertEquals(14, TesseraA11ySlots.BOUNDS_H);
    }

    @Test
    public void theCollectionPositionFollowsTheBounds() {
        // 1-based position and set size, the pair `aria-posinset`/`aria-setsize` need.
        assertEquals(15, TesseraA11ySlots.POS_IN_SET_SLOT);
        assertEquals(16, TesseraA11ySlots.SET_SIZE_SLOT);
    }

    @Test
    public void theActionBlockStartsWhereTheCollectionEnds() {
        // `actionCountIndex` is the count slot; the ids follow it. Getting this wrong makes the count
        // and the first id the same slot, which is silent and always wrong.
        assertEquals(17, TesseraA11ySlots.actionCountIndex());
        assertEquals(18, TesseraA11ySlots.actionIndex(0));
        assertEquals(25, TesseraA11ySlots.actionIndex(7)); // the eighth and last action
    }

    @Test
    public void theChildBlockFollowsTheActions() {
        assertEquals(26, TesseraA11ySlots.childCountIndex());
        assertEquals(27, TesseraA11ySlots.childIndex(0));
        assertEquals(90, TesseraA11ySlots.childIndex(63)); // the last of 64 children
    }

    @Test
    public void theArrayIsLargeEnoughForItsWorstCase() {
        // The last child's slot must be the last index. A one-off here is the classic buffer overflow
        // that JNI turns into memory corruption rather than an exception.
        assertEquals(91, TesseraA11ySlots.slotCount());
        assertTrue(
                "the last action id fits",
                TesseraA11ySlots.actionIndex(TesseraA11ySlots.MAX_ACTIONS - 1)
                        < TesseraA11ySlots.childCountIndex());
        assertTrue(
                "the last child id fits",
                TesseraA11ySlots.childIndex(TesseraA11ySlots.MAX_CHILDREN - 1)
                        < TesseraA11ySlots.slotCount());
    }

    @Test
    public void unpackRoundsHalfUp() {
        // 1/64 px fixed point. **`Math.round` rounds half UP, not away from zero**: -2.5 becomes -2,
        // not -3. That asymmetry is Java's, not this code's, and pinning it is the point - an earlier
        // version of this test asserted -3 and failed, which is exactly the mistake a reader would
        // make too. A whole pixel is exact either way, so nothing a user sees depends on it.
        // Exact values, not guessed ones: `Math.round(v)` is `floor(v + 0.5)`, so a positive half
        // rounds up and a negative half rounds toward zero. I got three of these wrong by reasoning
        // about them before computing them - which is why the expected numbers are written as the
        // result of that definition and each one is stated in pixels.
        assertEquals(0, TesseraA11ySlots.unpack(0)); // 0
        assertEquals(64, TesseraA11ySlots.unpack(64 * 64)); // 64
        assertEquals(3, TesseraA11ySlots.unpack(64 * 2 + 32)); // 2.5 -> 3
        assertEquals(-2, TesseraA11ySlots.unpack(-(64 * 2 + 32))); // -2.5 -> -2, not -3
        assertEquals(2, TesseraA11ySlots.unpack(96)); // 1.5 -> 2
        assertEquals(-1, TesseraA11ySlots.unpack(-96)); // -1.5 -> -1
        assertEquals(1, TesseraA11ySlots.unpack(32)); // 0.5 -> 1
        assertEquals(0, TesseraA11ySlots.unpack(-32)); // -0.5 -> 0
        // The native packer rounds half away from zero, so a negative half pixel differs by one
        // between the two sides. It is invisible at device density, and recorded so nobody "fixes"
        // the Java to match and moves a golden.
        assertEquals(0, TesseraA11ySlots.unpack(-32));
    }

    @Test
    public void onlySetTextCarriesAStringPayload() {
        // `ACTION_SET_SELECTION` carries a start/end pair, not a string. A bridge that forwarded it as
        // text would hand the engine a number it would apply as replacement content.
        final int setText = 2097152; // AccessibilityNodeInfo.ACTION_SET_TEXT
        final int setSelection = 131072; // ACTION_SET_SELECTION
        assertTrue(TesseraA11ySlots.carriesTextPayload(setText, setText));
        assertFalse(TesseraA11ySlots.carriesTextPayload(setSelection, setText));
    }

    @Test
    public void theCapsMatchTheEngine() {
        // Mirrors `ui::a11y::kMaxChildren` and `hal::android::kMaxActions`. Both are the native side's
        // bounds; a Java value larger than the engine's would index past what `nativeDescribe` wrote.
        assertEquals(64, TesseraA11ySlots.MAX_CHILDREN);
        assertEquals(8, TesseraA11ySlots.MAX_ACTIONS);
    }
}
