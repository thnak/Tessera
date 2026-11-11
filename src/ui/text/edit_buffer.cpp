// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "tessera/ui/text/edit_buffer.hpp"

#include <algorithm>
#include <utility>

namespace tessera::ui::text {

EditBuffer::EditBuffer(std::size_t max_bytes)
    : max_bytes_(max_bytes == 0 ? kMaxEditTextBytes : max_bytes) {}

EditBuffer::EditBuffer(std::string_view initial_text, std::size_t max_bytes)
    : max_bytes_(max_bytes == 0 ? kMaxEditTextBytes : max_bytes) {
    set_text(initial_text);
}

bool EditBuffer::reserve(std::size_t bytes) {
    if (bytes > max_bytes_) {
        return false;
    }
    text_.reserve(bytes);
    return true;
}

EditOffset EditBuffer::snap(EditOffset offset) const {
    return edit_detail::snap_to_cluster(text_, offset);
}

std::string_view EditBuffer::selected_text() const {
    const EditOffset start = selection_start();
    const EditOffset end = selection_end();
    return std::string_view(text_).substr(start, end - start);
}

CaretPosition EditBuffer::caret_position() const {
    // Count forward from the memoized prefix; a caret before it recounts from the start.
    if (counted_.offset > caret_ || counted_.offset > text_.size() ||
        (counted_.verify &&
         edit_detail::snap_to_cluster(text_, counted_.offset) != counted_.offset)) {
        counted_ = CountedPrefix{};
    }
    counted_.verify = false;
    const EditOffset limit = caret_ > text_.size() ? text_.size() : caret_;
    EditOffset cursor = counted_.offset;
    std::uint32_t partial_graphemes = 0;
    std::uint32_t partial_codepoints = 0;
    while (cursor < limit) {
        const EditOffset next = edit_detail::next_cluster(text_, cursor);
        if (next > limit) {
            // The caret sits inside a cluster (an insert can leave it there): the position counts
            // that cluster and the code points before the caret, but the memo keeps only whole
            // clusters so it stays valid for any later caret.
            partial_graphemes = 1;
            for (EditOffset at = cursor; at < limit; at = edit_detail::next_codepoint(text_, at)) {
                ++partial_codepoints;
            }
            break;
        }
        ++counted_.graphemes;
        for (EditOffset at = cursor; at < next; at = edit_detail::next_codepoint(text_, at)) {
            ++counted_.codepoints;
        }
        cursor = next;
    }
    counted_.offset = cursor;
    CaretPosition position;
    position.bytes = caret_;
    position.codepoints = counted_.codepoints + partial_codepoints;
    position.graphemes = counted_.graphemes + partial_graphemes;
    return position;
}

void EditBuffer::note_edit(EditOffset from) {
    // The text before `from` is unchanged, and so is the boundary at counted_.offset when it lies
    // before `from`. A memo ending exactly at `from` stays only if that is still a boundary after
    // the edit (an insertion there can extend the cluster before it), which the next query checks.
    // A memo beyond `from` describes text that no longer exists.
    if (counted_.offset < from) {
        return;
    }
    if (counted_.offset == from) {
        counted_.verify = true;
        return;
    }
    counted_ = CountedPrefix{};
}

EditResult EditBuffer::snapshot(EditStatus status, bool changed, bool truncated) const {
    EditResult result;
    result.status = status;
    result.changed = changed;
    result.truncated = truncated;
    result.caret = caret_position();
    result.anchor = anchor_;
    return result;
}

void EditBuffer::clamp_selection() {
    caret_ = snap(caret_);
    anchor_ = snap(anchor_);
}

EditResult EditBuffer::set_text(std::string_view text) {
    if (!edit_detail::valid_utf8(text)) {
        return snapshot(EditStatus::InvalidUtf8, false);
    }
    const std::string normalized = edit_detail::normalize_newlines(text);
    std::string_view stored = normalized;
    bool truncated = false;
    if (stored.size() > max_bytes_) {
        // Cut at a cluster boundary so the stored text ends on a code-point/cluster edge.
        EditOffset cut = 0;
        while (cut < stored.size()) {
            const EditOffset next = edit_detail::next_cluster(stored, cut);
            if (next > max_bytes_) {
                break;
            }
            cut = next;
        }
        stored = stored.substr(0, cut);
        truncated = true;
    }
    const bool changed = stored != text_;
    counted_ = CountedPrefix{};
    text_.assign(stored.data(), stored.size());
    caret_ = text_.size();
    anchor_ = caret_;
    if (!changed) {
        return snapshot(truncated ? EditStatus::Truncated : EditStatus::NoChange, false, truncated);
    }
    return snapshot(truncated ? EditStatus::Truncated : EditStatus::Ok, true, truncated);
}

EditResult EditBuffer::set_caret(EditOffset offset) {
    const EditOffset next = snap(offset);
    const bool changed = next != caret_ || anchor_ != next;
    caret_ = next;
    anchor_ = next;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::set_selection(EditOffset anchor, EditOffset focus) {
    const EditOffset next_anchor = snap(anchor);
    const EditOffset next_focus = snap(focus);
    const bool changed = next_anchor != anchor_ || next_focus != caret_;
    anchor_ = next_anchor;
    caret_ = next_focus;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::select_all() {
    const bool changed = anchor_ != 0 || caret_ != text_.size();
    anchor_ = 0;
    caret_ = text_.size();
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::clear_selection() {
    const bool changed = has_selection();
    anchor_ = caret_;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::insert(std::string_view text) {
    return replace_range(selection_start(), selection_end(), text);
}

EditResult EditBuffer::replace_range(EditOffset start, EditOffset end, std::string_view text) {
    if (!edit_detail::valid_utf8(text)) {
        return snapshot(EditStatus::InvalidUtf8, false);
    }
    EditOffset from = snap(start);
    EditOffset to = snap(end);
    if (to < from) {
        std::swap(from, to);
    }
    const std::string normalized = edit_detail::normalize_newlines(text);
    const std::size_t removed = to - from;
    const std::size_t base = text_.size() - removed;
    const std::size_t room = max_bytes_ > base ? max_bytes_ - base : 0;

    std::string_view inserted = normalized;
    bool truncated = false;
    if (inserted.size() > room) {
        EditOffset cut = 0;
        while (cut < inserted.size()) {
            const EditOffset next = edit_detail::next_cluster(inserted, cut);
            if (next > room) {
                break;
            }
            cut = next;
        }
        inserted = inserted.substr(0, cut);
        truncated = true;
    }
    if (inserted.empty()) {
        if (from == to) {
            caret_ = from;
            anchor_ = from;
            return snapshot(truncated ? EditStatus::Truncated : EditStatus::NoChange, false,
                            truncated);
        }
        note_edit(from);
        text_.erase(from, removed);
        caret_ = from;
        anchor_ = from;
        return snapshot(EditStatus::Ok, true, truncated);
    }

    note_edit(from);
    text_.replace(from, removed, inserted);
    caret_ = from + inserted.size();
    anchor_ = caret_;
    if (truncated) {
        return snapshot(EditStatus::Truncated, true, true);
    }
    return snapshot(EditStatus::Ok, true, false);
}

EditResult EditBuffer::delete_backward(bool by_word) {
    if (has_selection()) {
        return replace_range(selection_start(), selection_end(), {});
    }
    if (caret_ == 0) {
        return snapshot(EditStatus::NoChange, false);
    }
    const EditOffset from =
        by_word ? edit_detail::word_left(text_, caret_) : edit_detail::prev_cluster(text_, caret_);
    return replace_range(from, caret_, {});
}

EditResult EditBuffer::delete_forward(bool by_word) {
    if (has_selection()) {
        return replace_range(selection_start(), selection_end(), {});
    }
    if (caret_ >= text_.size()) {
        return snapshot(EditStatus::NoChange, false);
    }
    const EditOffset to =
        by_word ? edit_detail::word_right(text_, caret_) : edit_detail::next_cluster(text_, caret_);
    return replace_range(caret_, to, {});
}

EditResult EditBuffer::move_left(bool by_word, bool extend) {
    const EditOffset previous_caret = caret_;
    const EditOffset previous_anchor = anchor_;
    if (!extend && has_selection()) {
        caret_ = selection_start();
        anchor_ = caret_;
    } else {
        caret_ = by_word ? edit_detail::word_left(text_, caret_)
                         : edit_detail::prev_cluster(text_, caret_);
        if (!extend) {
            anchor_ = caret_;
        }
    }
    const bool changed = caret_ != previous_caret || anchor_ != previous_anchor;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::move_right(bool by_word, bool extend) {
    const EditOffset previous_caret = caret_;
    const EditOffset previous_anchor = anchor_;
    if (!extend && has_selection()) {
        caret_ = selection_end();
        anchor_ = caret_;
    } else {
        caret_ = by_word ? edit_detail::word_right(text_, caret_)
                         : edit_detail::next_cluster(text_, caret_);
        if (!extend) {
            anchor_ = caret_;
        }
    }
    const bool changed = caret_ != previous_caret || anchor_ != previous_anchor;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::move_home(bool document, bool extend) {
    const EditOffset previous_caret = caret_;
    const EditOffset previous_anchor = anchor_;
    caret_ = document ? 0 : edit_detail::line_start(text_, caret_);
    if (!extend) {
        anchor_ = caret_;
    }
    const bool changed = caret_ != previous_caret || anchor_ != previous_anchor;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}

EditResult EditBuffer::move_end(bool document, bool extend) {
    const EditOffset previous_caret = caret_;
    const EditOffset previous_anchor = anchor_;
    caret_ = document ? text_.size() : edit_detail::line_end(text_, caret_);
    if (!extend) {
        anchor_ = caret_;
    }
    const bool changed = caret_ != previous_caret || anchor_ != previous_anchor;
    return snapshot(changed ? EditStatus::Ok : EditStatus::NoChange, changed);
}
} // namespace tessera::ui::text
