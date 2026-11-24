// SPDX-License-Identifier: MIT
//
// Selection over static text (issue #279). See `selection_controller.hpp` for the shape and why it
// is separate from `EditController`.

#include "tessera/ui/text/selection_controller.hpp"

#include <algorithm>

namespace tessera::ui::text {

std::string_view SelectionRange::of(std::string_view text) const noexcept {
    if (!is_readable()) {
        return {};
    }
    // A text that shrank between the press and the read is clamped rather than asserted: text can
    // change under a gesture (a label bound to a cell, a locale switch), and a `substr` past the
    // end is undefined behaviour for a case the caller cannot prevent.
    const std::size_t begin = std::min(start.offset, text.size());
    const std::size_t end_offset = std::min(end.offset, text.size());
    if (end_offset <= begin) {
        return {};
    }
    return text.substr(begin, end_offset - begin);
}

std::size_t SelectionController::caret_at(const SelectionTarget& target) {
    const TextHit hit = hit_test_point(target.text, target.style, target.constraints, target.x,
                                       target.y, target.metrics);
    return hit.byte_offset;
}

SelectionGesture SelectionController::press(const SelectionTarget& target) {
    clear();
    if (target.node == 0 || target.text.empty() || !target.selectable) {
        // `none`, an empty label, or no text under the pointer: the gesture starts nothing. The
        // caller can still drag, and every `drag` will be ignored, which is what CSS does.
        return SelectionGesture::Ignored;
    }
    if (target.user_select == UserSelect::All) {
        // The whole element, without a hit test: the answer is the element, so measuring where the
        // press landed would only be a way to reach the same result by a longer route.
        gesture_ = SelectionGesture::All;
        anchor_ = SelectionPoint{.node = target.node, .offset = 0};
        range_.start = anchor_;
        range_.end = SelectionPoint{.node = target.node, .offset = target.text.size()};
        last_ = range_;
        return SelectionGesture::All;
    }
    gesture_ = SelectionGesture::Caret;
    const std::size_t offset = caret_at(target);
    anchor_ = SelectionPoint{.node = target.node, .offset = offset};
    // A press with no drag is a collapsed caret, not a selection: `is_readable()` is false for it,
    // so a copy attempted before the pointer moves copies nothing. That is what CSS does, and it
    // keeps "clicked in a paragraph" from replacing the clipboard with one character.
    range_.start = anchor_;
    range_.end = anchor_;
    last_ = range_;
    return SelectionGesture::Caret;
}

bool SelectionController::drag(const SelectionTarget& target) {
    if (gesture_ == SelectionGesture::Ignored || range_.start.node == 0) {
        return false;
    }
    // `user-select: all` holds the whole element for the gesture: a drag inside it must not shrink
    // the selection to two carets, which is the one way `all` differs from a press that happened to
    // cover the text.
    if (gesture_ == SelectionGesture::All) {
        return false;
    }
    // The drag must stay in the anchor's node. A pointer that leaves a label for a different one
    // would otherwise produce a range addressing bytes of a string the anchor never named, which
    // `is_readable()` refuses but which would silently end the gesture - better to ignore the move
    // and keep the selection the user can see.
    if (target.node != anchor_.node) {
        return false;
    }
    // And the policy is re-read on every move, not only at the press: a drag that enters a
    // `user-select: none` node - or a subtree whose value changed under the gesture - must not
    // extend into it.
    if (!target.selectable) {
        return false;
    }
    const std::size_t offset = caret_at(target);
    // Byte order, not drag order: a right-to-left drag reports the same range as a left-to-right
    // one over the same characters, so a copy never depends on which way the pointer travelled.
    SelectionRange next{};
    if (offset <= anchor_.offset) {
        next.start = SelectionPoint{.node = anchor_.node, .offset = offset};
        next.end = anchor_;
    } else {
        next.start = anchor_;
        next.end = SelectionPoint{.node = anchor_.node, .offset = offset};
    }
    if (next.start.offset == last_.start.offset && next.end.offset == last_.end.offset &&
        next.start.node == last_.start.node && next.end.node == last_.end.node) {
        return false;
    }
    range_ = next;
    last_ = next;
    return true;
}

void SelectionController::release() noexcept {
    // The range stays: a released selection is still a selection, which is what makes `Ctrl+C`
    // after a drag mean anything. `dragging()` goes false because the gesture is over, not because
    // the range went away.
    gesture_ = SelectionGesture::Ignored;
}

void SelectionController::clear() noexcept {
    gesture_ = SelectionGesture::Ignored;
    range_ = SelectionRange{};
    last_ = SelectionRange{};
    anchor_ = SelectionPoint{};
}

std::string_view copy_text(std::string_view text, const SelectionRange& range) noexcept {
    // `none` text never reaches a range: `press` refuses to start one on it and `drag` re-checks on
    // every move. So the only rule this function adds is the shape one - an unreadable range copies
    // nothing rather than the whole string, which is the failure mode a careless `substr` would
    // have.
    return range.of(text);
}

} // namespace tessera::ui::text
