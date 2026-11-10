// SPDX-License-Identifier: MIT
//
// Collection semantics for one virtualized collection (`docs/specs/virtualization-v1.md` section
// 7): set size and positions from the key index, realized-item exposure, the active descendant,
// the `(UiId, ItemKey)` automation identity, selection in data order, and coalesced announcements.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/collection_semantics.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace tessera::ui::virtualization {

namespace {

bool by_index(const CollectionItemView& lhs, const CollectionItemView& rhs) noexcept {
    return lhs.index < rhs.index;
}

bool selection_by_index(const SelectionEntry& lhs, const SelectionEntry& rhs) noexcept {
    return lhs.index < rhs.index;
}

// Appends into `out` and advances it. Truncated output is the documented behavior: the caller
// sizes the buffer, and no path here allocates.
void append_text(std::span<char>& out, std::string_view text) noexcept {
    const std::size_t count = std::min(out.size(), text.size());
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = text[index];
    }
    out = out.subspan(count);
}

void append_size(std::span<char>& out, std::size_t value) noexcept {
    if (out.empty()) {
        return;
    }
    const std::to_chars_result result = std::to_chars(out.data(), out.data() + out.size(), value);
    const std::size_t written =
        result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - out.data()) : out.size();
    out = out.subspan(written);
}

} // namespace

CollectionSemantics::CollectionSemantics(VirtualRange& range)
    : range_(range),
      realized_scratch_(range.recycler().capacity()),
      selection_scratch_(range.item_state().capacity()) {
    // Baseline the observed state: the first `observe()` must not re-announce focus or selection
    // that existed before the semantics object was created.
    has_observed_focus_ = range_.has_focus();
    observed_focus_ = has_observed_focus_ ? range_.focused_key() : VirtualItemId{};
    observed_selection_revision_ = range_.item_state().selection_revision();
}

std::optional<std::size_t> CollectionSemantics::pos_in_set(std::size_t index) const noexcept {
    if (index >= range_.item_count()) {
        return std::nullopt;
    }
    return index + kPosInSetBase;
}

std::optional<std::size_t> CollectionSemantics::pos_in_set(VirtualItemId key) const noexcept {
    const std::optional<std::size_t> index = range_.index_of(key);
    if (!index) {
        return std::nullopt;
    }
    return *index + kPosInSetBase;
}

std::size_t CollectionSemantics::realized_items(std::span<CollectionItemView> out) const noexcept {
    std::size_t written = 0;
    for (std::size_t position = 0; position < range_.realized_count(); ++position) {
        if (written == realized_scratch_.size()) {
            break; // defensive: the scratch is sized for the pool cap
        }
        const std::optional<std::size_t> realized_index = range_.realized_index_at(position);
        if (!realized_index) {
            continue;
        }
        const VirtualItemId key = range_.key_at(*realized_index);
        // The realized table and the key index are written together by `sync`/`update`; resolving
        // the key again keeps the exposed position derived from the key index, which is the
        // authority for `pos_in_set` (spec section 7).
        const std::optional<std::size_t> index = range_.index_of(key);
        if (!index) {
            continue;
        }
        const ui::Node* node = range_.realized_node(*realized_index);
        if (node == nullptr) {
            continue;
        }
        CollectionItemView& view = realized_scratch_[written++];
        view.identity = CollectionItemIdentity{node->automation_id(), key};
        view.index = *index;
        view.pos_in_set = *index + kPosInSetBase;
        view.set_size = range_.item_count();
        view.focused = range_.is_focused(key);
        view.selected = range_.is_selected(key);
    }
    std::sort(realized_scratch_.begin(),
              realized_scratch_.begin() + static_cast<std::ptrdiff_t>(written), by_index);
    const std::size_t copied = std::min(written, out.size());
    std::copy_n(realized_scratch_.begin(), copied, out.begin());
    return copied;
}

automation::UiId CollectionSemantics::ui_id_of(VirtualItemId key) const noexcept {
    const std::optional<std::size_t> index = range_.index_of(key);
    if (!index) {
        return 0;
    }
    const ui::Node* node = range_.realized_node(*index);
    return node != nullptr ? node->automation_id() : 0;
}

std::optional<CollectionItemIdentity>
CollectionSemantics::identity_of(VirtualItemId key) const noexcept {
    const std::optional<std::size_t> index = range_.index_of(key);
    if (!index) {
        return std::nullopt;
    }
    const ui::Node* node = range_.realized_node(*index);
    if (node == nullptr) {
        return std::nullopt;
    }
    return CollectionItemIdentity{node->automation_id(), key};
}

std::optional<CollectionItemIdentity> CollectionSemantics::active_descendant() const noexcept {
    if (!range_.has_focus()) {
        return std::nullopt;
    }
    return identity_of(range_.focused_key());
}

std::optional<std::size_t> CollectionSemantics::focused_pos_in_set() const noexcept {
    if (!range_.has_focus()) {
        return std::nullopt;
    }
    return pos_in_set(range_.focused_key());
}

std::size_t CollectionSemantics::selection(std::span<SelectionEntry> out) const noexcept {
    std::size_t written = 0;
    range_.item_state().for_each_selected([this, &written](VirtualItemId key) {
        if (written == selection_scratch_.size()) {
            return;
        }
        const std::optional<std::size_t> index = range_.index_of(key);
        if (!index) {
            return; // a key the last sync no longer has is not part of the ordered selection
        }
        const ui::Node* node = range_.realized_node(*index);
        SelectionEntry& entry = selection_scratch_[written++];
        entry.identity = CollectionItemIdentity{node != nullptr ? node->automation_id() : 0, key};
        entry.index = *index;
        entry.pos_in_set = *index + kPosInSetBase;
    });
    std::sort(selection_scratch_.begin(),
              selection_scratch_.begin() + static_cast<std::ptrdiff_t>(written),
              selection_by_index);
    const std::size_t copied = std::min(written, out.size());
    std::copy_n(selection_scratch_.begin(), copied, out.begin());
    return copied;
}

void CollectionSemantics::observe() noexcept {
    const bool has_focus = range_.has_focus();
    const VirtualItemId focus_key = has_focus ? range_.focused_key() : VirtualItemId{};
    if (has_focus != has_observed_focus_ || focus_key != observed_focus_) {
        has_observed_focus_ = has_focus;
        observed_focus_ = focus_key;
        ++stats_.focus_changes;
        // Clearing focus has no item to name, so it produces no announcement: the consumer owns
        // the "focus left the collection" message.
        if (has_focus) {
            stage(AnnouncementKind::Focus, focus_key, false);
        }
    }

    const std::uint64_t revision = range_.item_state().selection_revision();
    if (revision != observed_selection_revision_) {
        observed_selection_revision_ = revision;
        ++stats_.selection_changes;
        const ItemStateStore::SelectionChange change = range_.item_state().last_selection_change();
        if (change.valid) {
            // A key that a data update dropped no longer resolves and stages nothing.
            stage(AnnouncementKind::Selection, change.key, change.selected);
        }
    }
}

void CollectionSemantics::stage(AnnouncementKind kind, VirtualItemId key, bool selected) noexcept {
    const std::optional<std::size_t> index = range_.index_of(key);
    if (!index) {
        return;
    }
    const ui::Node* node = range_.realized_node(*index);
    Announcement announcement;
    announcement.kind = kind;
    announcement.identity =
        CollectionItemIdentity{node != nullptr ? node->automation_id() : 0, key};
    announcement.index = *index;
    announcement.pos_in_set = *index + kPosInSetBase;
    announcement.set_size = range_.item_count();
    announcement.selected = selected;
    announcement.realized = node != nullptr;
    announcement.sequence = next_sequence_++;
    ++stats_.staged;

    // Coalescing policy (spec section 7): an undrained announcement of the same kind is superseded
    // in place, so a burst of changes between two observations still exposes exactly one entry per
    // kind. The ring is bounded by kMaxPendingAnnouncements; when another kind fills it, the oldest
    // entry is dropped instead of growing the object.
    for (std::size_t offset = 0; offset < pending_count_; ++offset) {
        const std::size_t slot = (pending_head_ + offset) % kMaxPendingAnnouncements;
        if (pending_[slot].kind == kind) {
            pending_[slot] = announcement;
            ++stats_.superseded;
            return;
        }
    }
    if (pending_count_ == kMaxPendingAnnouncements) {
        pending_head_ = (pending_head_ + 1) % kMaxPendingAnnouncements;
        --pending_count_;
        ++stats_.dropped;
    }
    pending_[(pending_head_ + pending_count_) % kMaxPendingAnnouncements] = announcement;
    ++pending_count_;
}

std::optional<Announcement> CollectionSemantics::take_announcement() noexcept {
    if (pending_count_ == 0) {
        return std::nullopt;
    }
    const Announcement announcement = pending_[pending_head_];
    pending_head_ = (pending_head_ + 1) % kMaxPendingAnnouncements;
    --pending_count_;
    ++stats_.taken;
    return announcement;
}

std::string_view CollectionSemantics::format_item_key(VirtualItemId key,
                                                      std::span<char> out) noexcept {
    if (out.empty()) {
        return {};
    }
    const std::to_chars_result result =
        std::to_chars(out.data(), out.data() + out.size(), key.value);
    if (result.ec != std::errc{}) {
        return {}; // the buffer cannot hold a 64-bit decimal value
    }
    return std::string_view(out.data(), static_cast<std::size_t>(result.ptr - out.data()));
}

std::string_view CollectionSemantics::format_announcement(const Announcement& announcement,
                                                          std::span<char> out) noexcept {
    if (out.empty()) {
        return {};
    }
    std::span<char> remaining = out;
    switch (announcement.kind) {
    case AnnouncementKind::Focus:
        append_text(remaining, "focus item ");
        break;
    case AnnouncementKind::Selection:
        append_text(remaining, announcement.selected ? "select item " : "deselect item ");
        break;
    }
    append_size(remaining, announcement.pos_in_set);
    append_text(remaining, " of ");
    append_size(remaining, announcement.set_size);
    return std::string_view(out.data(), out.size() - remaining.size());
}

} // namespace tessera::ui::virtualization
