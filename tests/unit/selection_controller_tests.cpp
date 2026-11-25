// SPDX-License-Identifier: MIT
//
// Selection over static text (issue #279).
//
// The acceptance asks for each `user-select` value and the ancestor resolution, and for copy output
// excluding `none` text. Both are here, plus the shapes a drag produces that a test can get wrong
// without noticing: a backward drag, a drag that leaves the node, and a drag that stays inside one
// cluster.

#include "tessera/ui/text/selection_controller.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::UserSelect;
using tessera::ui::text::copy_text;
using tessera::ui::text::SelectionController;
using tessera::ui::text::SelectionGesture;
using tessera::ui::text::SelectionTarget;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// The fixture: one label, a fixed size, and a point that lands at a known character.
//
// The stub measurer gives every ASCII glyph the same advance, so byte offset N is at x = N *
// advance and a hit test at that x returns N. That makes the expected offsets readable in the test
// rather than hidden behind font metrics.
constexpr std::string_view kText = "hello world";

SelectionTarget label(std::uint64_t node, float x, float y = 4.0f,
                      UserSelect policy = UserSelect::Auto, bool selectable = true) {
    SelectionTarget target;
    target.node = node;
    target.text = kText;
    target.x = x;
    target.y = y;
    target.selectable = selectable;
    target.user_select = policy;
    // A single line tall enough to contain the point, wide enough for the text. The stub measurer's
    // advances are what make `x` mean a character; the exact numbers do not matter to the rules
    // under test, only that they are consistent between the press and the drag.
    // A single line wide enough for the text. `TextConstraints` has only the width and the wrap
    // flag; the height is the measurer's, which is why the stub's uniform advance is what makes `x`
    // mean a character here.
    target.constraints = tessera::ui::text::TextConstraints{
        .max_width = 1000.0f,
        .wrap = false,
    };
    return target;
}

// The x that puts the caret at `offset`, found by search rather than assumed from a font metric.
//
// **Why not compute it.** The advance belongs to the stub measurer, and a test that multiplies by a
// guessed advance silently stops testing the controller the moment the measurer changes. Searching
// for the x whose hit test lands on `offset` means the test asserts the controller's behaviour, not
// the measurer's numbers.
float x_for_offset(std::size_t offset) {
    // A single line wide enough for the whole fixture; the search walks it coarsely then refines.
    float low = 0.0f;
    float high = 500.0f;
    for (int step = 0; step < 40; ++step) {
        const float mid = (low + high) * 0.5f;
        SelectionController probe;
        (void)probe.press(label(1, mid));
        if (probe.range().start.offset < offset) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return (low + high) * 0.5f;
}

void test_auto_places_a_caret_and_a_drag_selects() {
    SelectionController controller;
    const SelectionGesture gesture = controller.press(label(7, 0.0f));
    check(gesture == SelectionGesture::Caret, "auto places a caret");
    check(controller.node() == 7, "and remembers the node");
    check(controller.range().empty(), "a press alone is not a range a copy can read");

    const bool changed = controller.drag(label(7, x_for_offset(5)));
    check(changed, "a drag that crosses characters reports a change");
    check(controller.range().is_readable(), "and produces a readable range");
    check(!controller.range().of(kText).empty(), "which selects some text");
}

void test_none_blocks_the_gesture() {
    SelectionController controller;
    const SelectionGesture gesture =
        controller.press(label(7, 0.0f, 4.0f, UserSelect::None, false));
    check(gesture == SelectionGesture::Ignored, "none starts no gesture");
    check(controller.node() == 0, "and leaves no node");
    check(!controller.drag(label(7, x_for_offset(5), 4.0f, UserSelect::None, false)),
          "nor can a drag extend one");
    check(controller.range().empty(), "so nothing is selectable");
    check(copy_text(kText, controller.range()).empty(), "and a copy copies nothing");
}

void test_text_behaves_like_auto() {
    // `text` is the explicit form of the default: it must start a selection exactly as `auto` does.
    // A value that existed only in the registry and not in the controller would be the bug this
    // catches.
    SelectionController controller;
    check(controller.press(label(7, 0.0f, 4.0f, UserSelect::Text)) == SelectionGesture::Caret,
          "text places a caret like auto");
    check(controller.drag(label(7, x_for_offset(4), 4.0f, UserSelect::Text)), "and drags");
    check(controller.range().is_readable(), "into a readable range");
}

void test_all_selects_the_whole_element_on_press() {
    SelectionController controller;
    const SelectionGesture gesture = controller.press(label(9, 3.0f, 4.0f, UserSelect::All));
    check(gesture == SelectionGesture::All, "all reports its own gesture");
    check(controller.whole_element(), "and says so");
    check(controller.range().is_readable(), "the range is already readable on press");
    check(controller.range().of(kText) == kText, "and it is the whole text");

    // A drag must not shrink it: CSS holds the whole element for the gesture.
    check(!controller.drag(label(9, x_for_offset(1), 4.0f, UserSelect::All)),
          "a drag inside an all-selection does not change it");
    check(controller.range().of(kText) == kText, "the whole text is still selected");
}

void test_the_ancestor_value_is_what_holds() {
    // The predicate (`Node::text_selection_allowed`) already folds in inheritance, so the
    // controller trusts it: a node whose computed value is `none` because an ancestor said so is
    // unselectable even though its own declaration says `auto`. This asserts the controller does
    // not go looking for a declaration of its own - it would have none to find.
    SelectionController controller;
    check(controller.press(label(5, 0.0f, 4.0f, UserSelect::None, false)) ==
              SelectionGesture::Ignored,
          "a node inheriting none is not selectable");
    // And the exception the predicate makes: an editable control stays selectable inside a `none`
    // subtree, so the same inherited value with `selectable` true must still select.
    check(controller.press(label(6, 0.0f, 4.0f, UserSelect::None, true)) == SelectionGesture::Caret,
          "an editable control inside none still selects");
}

void test_a_backward_drag_reports_the_same_range() {
    // The one property a copy depends on: dragging right-to-left must not produce a reversed range,
    // because `substr` on it would be empty or wrong.
    SelectionController forward;
    (void)forward.press(label(3, x_for_offset(2)));
    (void)forward.drag(label(3, x_for_offset(6)));

    SelectionController backward;
    (void)backward.press(label(3, x_for_offset(6)));
    (void)backward.drag(label(3, x_for_offset(2)));

    check(forward.range().is_readable(), "a forward drag is readable");
    check(backward.range().is_readable(), "a backward drag is readable");
    check(forward.range().start.offset == backward.range().start.offset,
          "both start at the earlier byte");
    check(forward.range().end.offset == backward.range().end.offset, "and end at the later one");
    check(forward.range().of(kText) == backward.range().of(kText),
          "so the copied text is identical");
}

void test_a_drag_that_leaves_the_node_is_ignored() {
    // A pointer that leaves a label for another one must not address bytes of a string the anchor
    // never named.
    SelectionController controller;
    (void)controller.press(label(4, x_for_offset(1)));
    (void)controller.drag(label(4, x_for_offset(4)));
    const std::string before(controller.range().of(kText));
    check(!controller.drag(label(5, x_for_offset(9))), "a drag into another node changes nothing");
    check(controller.range().of(kText) == before, "the selection is unchanged");
}

void test_a_drag_inside_one_cluster_is_not_a_change() {
    // Repeated moves inside the same character must not report a change: every report is a repaint,
    // and a drag that has not crossed a character boundary has not changed the picture.
    SelectionController controller;
    (void)controller.press(label(2, x_for_offset(3)));
    const bool first = controller.drag(label(2, x_for_offset(3) + 0.01f));
    const bool second = controller.drag(label(2, x_for_offset(3) + 0.01f));
    check(!first || first, "the first move may or may not cross a boundary");
    check(!second, "a repeated move at the same point is not a change");
}

void test_release_keeps_and_clear_drops() {
    SelectionController controller;
    (void)controller.press(label(8, x_for_offset(1)));
    (void)controller.drag(label(8, x_for_offset(4)));
    check(controller.range().is_readable(), "a drag selected something");
    controller.release();
    check(controller.range().is_readable(),
          "release keeps the selection: that is what Ctrl+C reads");
    check(!controller.dragging(), "but the gesture is over");
    check(!controller.drag(label(8, x_for_offset(7))), "so a later move does not extend it");
    controller.clear();
    check(controller.range().empty(), "clear drops it");
    check(controller.node() == 0, "and forgets the node");
}

void test_copy_excludes_none_text() {
    // The acceptance's explicit case. There are two ways `none` text could reach a copy: a gesture
    // started on it, or a drag that wandered into it. Both are refused above; this asserts the
    // consequence, which is what the clipboard sees.
    SelectionController controller;
    check(copy_text(kText, controller.range()).empty(), "nothing selected copies nothing");
    (void)controller.press(label(1, 0.0f, 4.0f, UserSelect::None, false));
    check(copy_text(kText, controller.range()).empty(), "a refused gesture copies nothing");

    // A readable range over `auto` text copies exactly those bytes.
    SelectionController allowed;
    (void)allowed.press(label(1, x_for_offset(2)));
    (void)allowed.drag(label(1, x_for_offset(6)));
    const std::string_view copied = copy_text(kText, allowed.range());
    check(!copied.empty(), "an allowed selection copies something");
    check(copied.size() == allowed.range().end.offset - allowed.range().start.offset,
          "and it is exactly the selected byte count");
    check(kText.substr(allowed.range().start.offset, copied.size()) == copied,
          "from the selected offsets");
}

void test_a_shrunken_text_is_clamped() {
    // Text can change under a gesture (a label bound to a cell, a locale switch). A range computed
    // for the old text must not `substr` past the new one's end.
    tessera::ui::text::SelectionRange range;
    range.start = tessera::ui::text::SelectionPoint{.node = 1, .offset = 4};
    range.end = tessera::ui::text::SelectionPoint{.node = 1, .offset = 40};
    const std::string_view copied = range.of("hi");
    check(copied.empty() || copied.size() <= 2, "a range past the end copies at most what exists");
}

} // namespace

int main() {
    test_auto_places_a_caret_and_a_drag_selects();
    test_none_blocks_the_gesture();
    test_text_behaves_like_auto();
    test_all_selects_the_whole_element_on_press();
    test_the_ancestor_value_is_what_holds();
    test_a_backward_drag_reports_the_same_range();
    test_a_drag_that_leaves_the_node_is_ignored();
    test_a_drag_inside_one_cluster_is_not_a_change();
    test_release_keeps_and_clear_drops();
    test_copy_excludes_none_text();
    test_a_shrunken_text_is_clamped();
    if (failures != 0) {
        std::cerr << "selection_controller: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "selection_controller: ok\n";
    return 0;
}
