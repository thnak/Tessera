// SPDX-License-Identifier: MIT
//
// Selection over static text (issue #279, follow-up of #217/#259).
//
// **Why this is not `EditController`.** `EditController` owns a document a user can change: an edit
// buffer, a history, a composition state. Static text has none of that - it is a `Text` node's
// painted string, and selecting it must not make the node editable. Sharing `EditController` would
// give every selectable label an undo stack it can never use, and would blur the line the
// accessibility layer already draws between a text field and a label.
//
// **What it does own.** A press resolves to a caret (or, for `user-select: all`, to the whole
// range), a drag extends it, and the result is a pair of byte offsets in the node's text. Bytes,
// not characters: the same offsets `text::TextHit` and `TextSelection` already speak, so copying is
// a `substr` and a surrogate pair cannot be split by accident.
//
// **What it deliberately does not do.** It does not own the scene, the input router or the
// clipboard. A caller feeds it presses and moves and reads the range back, which is what lets the
// same controller run on the UI thread, in a test with a manual clock, or behind the Android bridge
// without a second implementation.
#pragma once

#include "tessera/ui/scene/node_types.hpp"
#include "tessera/ui/text/hit_test.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::ui::text {

// Where one end of a selection sits. The text is addressed by the node it came from, so a drag that
// leaves one `Text` node and enters another cannot silently produce a range spanning both - see
// `SelectionController::note_text` for what happens instead.
struct SelectionPoint {
    // The node the offset indexes. `0` means "none", which is what an empty selection is.
    std::uint64_t node = 0;
    // A byte offset in that node's text, snapped to a grapheme-cluster boundary by
    // `hit_test_point`.
    std::size_t offset = 0;
};

// The range a press-and-drag selected, in the node's text.
//
// `start` is always less than or equal to `end` in byte order, so a backward drag (right to left)
// reports the same range as a forward one. A caller that needs the drag's direction keeps it from
// its own input state; nothing here depends on it.
struct SelectionRange {
    SelectionPoint start{};
    SelectionPoint end{};
    // True when the two ends are in the same node and `start.offset != end.offset`, which is the
    // only shape a copy can read. A range that fails this is "no selection" for a caller's purposes
    // even though the drag happened.
    [[nodiscard]] bool is_readable() const noexcept {
        return start.node != 0 && start.node == end.node && start.offset != end.offset;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !is_readable();
    }
    // The selected text of `text` (the node's own string), or an empty view when the range is not
    // readable or does not fit `text`. Clamped rather than asserted: a text that shrank between the
    // press and the read would otherwise be a `substr` past its end.
    [[nodiscard]] std::string_view of(std::string_view text) const noexcept;
};

// How a press resolves, decided by `user-select` (stylesheet-v1 section 11, issue #247).
enum class SelectionGesture : std::uint8_t {
    // The press starts no selection: `user-select: none`, or no selectable text under the pointer.
    Ignored,
    // The press places a caret and a drag extends from it: `auto` and `text`.
    Caret,
    // The press selects the whole element's text: `user-select: all`.
    All,
};

// What the controller needs to know about the node under the pointer. Collected by the caller
// because only it can run the computed-style and bounds lookups; the controller stays free of the
// scene.
struct SelectionTarget {
    std::uint64_t node = 0;
    // The node's painted text, which the offset indexes and a copy reads.
    std::string_view text;
    // The point in the text's own local coordinates (x from the content-box left, y from its top).
    // The caller subtracts the node's world offset; the controller never converts.
    float x = 0.0f;
    float y = 0.0f;
    // `Node::text_selection_allowed()`, which already folds in the inherited `user-select` value
    // and the editable-control exception. A caller that passes `false` for a field's own node would
    // make it unselectable, which is what that predicate says not to do.
    bool selectable = true;
    // The resolved keyword, for the one difference `text_selection_allowed()` cannot express:
    // `all` still has to select the whole element rather than place a caret.
    UserSelect user_select = UserSelect::Auto;
    // Everything `hit_test_point` needs. `constraints` must be the ones the node was laid out with,
    // or a hit lands on the wrong character.
    TextStyle style{};
    TextConstraints constraints{};
    FontMetrics metrics{};
};

// One selection at a time, over one node's static text.
//
// The caller drives it: `press` on pointer down, `drag` on each move while the button is held,
// `release` when it is lifted. It is a value the caller owns and may reset per frame or per scene.
class SelectionController {
public:
    // Starts a gesture at `target`. Returns the gesture the `user-select` policy chose, so a caller
    // can tell "ignored" from "selected nothing".
    //
    // `user-select: all` selects the whole text and returns `SelectionGesture::All` without calling
    // the hit test: the whole element is the answer, so measuring where inside it the press landed
    // would only be a way to get the same result wrong.
    SelectionGesture press(const SelectionTarget& target);

    // Extends the current gesture to `target`. Ignored unless the press started a selection in the
    // same node while the policy allows it - which is what stops a drag that wanders out of a
    // `none` subtree from extending a selection into it.
    //
    // Returns whether the range changed. A drag that stays inside one cluster does not have to
    // repaint, and saying so is cheaper than making every move dirty the frame.
    bool drag(const SelectionTarget& target);

    // Ends the gesture. The selection stays: CSS keeps a selection after the button is released,
    // and a caller that wants it gone calls `clear()`.
    void release() noexcept;

    // Drops the selection and any gesture in progress.
    void clear() noexcept;

    // The current range, which is `SelectionRange{}` when nothing is selected.
    [[nodiscard]] const SelectionRange& range() const noexcept {
        return range_;
    }
    // The node the selection is in, or 0 when there is none.
    [[nodiscard]] std::uint64_t node() const noexcept {
        return range_.start.node;
    }
    // Whether a drag is in progress. A caller that wants to keep the selection after a release, or
    // to end it when the scene changes under it, reads this rather than inferring it.
    [[nodiscard]] bool dragging() const noexcept {
        return gesture_ != SelectionGesture::Ignored && range_.start.node != 0;
    }
    // Whether `press` selected the whole element rather than a caret, so a later drag must not
    // shrink it: CSS's `all` keeps the whole range for the gesture's duration.
    [[nodiscard]] bool whole_element() const noexcept {
        return gesture_ == SelectionGesture::All;
    }

private:
    // Places a caret from a `SelectionTarget`. Shared by `press` and `drag` so the two cannot
    // resolve a point differently.
    static std::size_t caret_at(const SelectionTarget& target);

    SelectionGesture gesture_ = SelectionGesture::Ignored;
    SelectionRange range_{};
    // The point the press placed, which a drag extends from. Kept apart from `range_.start` because
    // the anchor never moves while the range's other end does.
    SelectionPoint anchor_{};
    // The range as of the last change, so `drag` can answer "did this move change anything".
    SelectionRange last_{};
};

// The text a copy should put on the clipboard for `range`, given the node's text.
//
// **Why this is a free function and not a method.** The clipboard is the host's, and the rule "copy
// excludes `none` text" is about what the range may contain, not about who writes it. A host that
// builds its own copy payload calls this; the ones that go through the controller's range do not
// need to.
//
// An empty view means "copy nothing", which a host must treat as "do not touch the clipboard"
// rather than "write an empty string".
[[nodiscard]] std::string_view copy_text(std::string_view text,
                                         const SelectionRange& range) noexcept;

} // namespace tessera::ui::text
