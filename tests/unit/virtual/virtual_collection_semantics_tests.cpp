// SPDX-License-Identifier: MIT
//
// Collection semantics over `VirtualRange` (`docs/specs/virtualization-v1.md` sections 7, 11 and
// 12): set size and per-item `pos_in_set` from the key index, the `(UiId, ItemKey)` identity of
// each realized item, the active descendant across realize/recycle, selection in data order,
// allocation-free queries after warm-up, and deterministic results.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/collection_semantics.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::size_t g_allocations = 0;

using tessera::ui::Node;
using tessera::ui::automation::UiId;
using tessera::ui::virtualization::Announcement;
using tessera::ui::virtualization::AnnouncementKind;
using tessera::ui::virtualization::CollectionItemIdentity;
using tessera::ui::virtualization::CollectionItemView;
using tessera::ui::virtualization::CollectionSemantics;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::SelectionEntry;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;

constexpr float kMain = 32.0f;
constexpr float kViewport = 100.0f;
constexpr std::size_t kCount = 20;
constexpr std::uint64_t kKeyStep = 7; // key values are sparse: keys are identity, not position
constexpr std::uint64_t kBaseKey = 1000;
constexpr UiId kTemplateUiId = 0x51A7'0001ULL;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_collection_semantics: " << message << "\n";
    }
    return condition;
}

class RowSource final : public VirtualDataSource {
public:
    explicit RowSource(std::vector<std::uint64_t> initial_keys, UiId template_ui_id = kTemplateUiId)
        : keys(std::move(initial_keys)),
          template_ui_id(template_ui_id) {}

    [[nodiscard]] std::size_t item_count() const override {
        return keys.size();
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{keys[index]};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints& constraints) const override {
        return VirtualItemMeasure{kMain, constraints.cross_extent};
    }
    void build(std::size_t, Node& out) override {
        out.set_element_kind("row");
        out.set_automation_id(template_ui_id);
    }

    std::vector<std::uint64_t> keys;
    UiId template_ui_id = kTemplateUiId;
};

VirtualRange::Config range_config(std::size_t pool_cap = 16, std::size_t state_capacity = 64) {
    VirtualRange::Config config;
    config.capacity = 128;
    config.pool_cap = pool_cap;
    config.state_capacity = state_capacity;
    config.estimate_main = kMain;
    config.gap = 0.0f;
    config.overscan_main = 0.0f;
    config.cross_extent = 100.0f;
    return config;
}

std::vector<std::uint64_t> sparse_keys(std::size_t count, std::uint64_t base = kBaseKey) {
    std::vector<std::uint64_t> keys(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys[index] = base + index * kKeyStep;
    }
    return keys;
}

struct Snapshot {
    std::vector<CollectionItemView> realized;
    std::vector<SelectionEntry> selection;
    std::optional<CollectionItemIdentity> active;
    std::optional<std::size_t> focused_pos;
    std::size_t set_size = 0;
    std::vector<Announcement> announcements;
};

// One deterministic script of focus/selection/scroll steps. Indices are derived from step number
// only (no clock, no RNG), so two runs are directly comparable.
Snapshot run_script(std::size_t step_count) {
    RowSource source(sparse_keys(kCount));
    Node container;
    VirtualRange range(range_config(), source, container);
    Snapshot snapshot;
    if (!range.sync().has_value() || !range.update(0.0f, kViewport).has_value()) {
        return snapshot;
    }
    CollectionSemantics semantics(range);
    semantics.observe();
    std::array<CollectionItemView, 16> views{};
    std::array<SelectionEntry, 16> selection{};
    for (std::size_t step = 0; step < step_count; ++step) {
        const std::size_t index = (step * 7) % kCount;
        (void)range.focus_item(range.key_at(index), kViewport);
        if (step % 3 == 0) {
            (void)range.set_selected(range.key_at((step * 11) % kCount), true);
        }
        if (step % 5 == 0) {
            (void)range.set_selected(range.key_at((step * 3) % kCount), false);
        }
        semantics.observe();
        while (const std::optional<Announcement> announcement = semantics.take_announcement()) {
            snapshot.announcements.push_back(*announcement);
        }
        const std::size_t view_count = semantics.realized_items(views);
        snapshot.realized.assign(views.begin(),
                                 views.begin() + static_cast<std::ptrdiff_t>(view_count));
        const std::size_t selected_count = semantics.selection(selection);
        snapshot.selection.assign(selection.begin(),
                                  selection.begin() + static_cast<std::ptrdiff_t>(selected_count));
        snapshot.set_size = semantics.set_size();
    }
    snapshot.active = semantics.active_descendant();
    snapshot.focused_pos = semantics.focused_pos_in_set();
    return snapshot;
}

} // namespace

// Count allocations so the semantics queries and the announce path can be proven allocation-free
// after warm-up.
void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main() {
    RowSource source(sparse_keys(kCount));
    Node container;
    VirtualRange range(range_config(), source, container);
    if (!expect(range.sync().has_value(), "sync the sparse-key source") ||
        !expect(range.update(0.0f, kViewport).has_value(), "realize the top window")) {
        return 1;
    }
    CollectionSemantics semantics(range);
    semantics.observe(); // baseline: no pre-existing focus or selection to announce

    // Set-size/pos_in_set matrix with key gaps: identity comes from the sparse key, position from
    // the current data index, and the set size from the item count.
    if (!expect(semantics.set_size() == kCount, "set size is the item count") ||
        !expect(!semantics.pos_in_set(kCount).has_value(), "index at the count is out of range") ||
        !expect(!semantics.pos_in_set(VirtualItemId{4}).has_value(),
                "unknown key has no position") ||
        !expect(!semantics.focused_pos_in_set().has_value(),
                "no focus means no focused position")) {
        return 2;
    }
    for (std::size_t index = 0; index < kCount; ++index) {
        const VirtualItemId key = range.key_at(index);
        if (!expect(semantics.pos_in_set(index) == index + 1, "pos_in_set is the 1-based index") ||
            !expect(semantics.pos_in_set(key) == index + 1, "key position matches its index") ||
            !expect(!semantics.focused_pos_in_set().has_value(), "no focus during the scan")) {
            return 3;
        }
    }

    // Realized exposure: data order, positions, set size, and the automation identity read off the
    // realized node (never re-hashed).
    std::array<CollectionItemView, 16> views{};
    const std::size_t realized = semantics.realized_items(views);
    if (!expect(realized == range.realized_count() && realized == 4, "four items are realized") ||
        !expect(views[0].index == 0 && views[3].index == 3, "realized views follow data order")) {
        return 4;
    }
    for (std::size_t position = 0; position < realized; ++position) {
        const CollectionItemView& view = views[position];
        if (!expect(view.index == position, "data order matches the position") ||
            !expect(view.pos_in_set == position + 1, "realized pos_in_set is 1-based") ||
            !expect(view.set_size == kCount, "realized view carries the set size") ||
            !expect(view.identity.item_key == range.key_at(position), "identity carries the key") ||
            !expect(view.identity.ui_id == kTemplateUiId, "identity carries the node UiId") ||
            !expect(!view.focused && !view.selected, "no focus or selection yet")) {
            return 5;
        }
    }
    std::array<CollectionItemView, 2> short_views{};
    if (!expect(semantics.realized_items(short_views) == 2, "short buffer exposes a prefix") ||
        !expect(short_views[0].index == 0 && short_views[1].index == 1,
                "short buffer keeps data order") ||
        !expect(semantics.realized_items({}) == 0, "empty buffer exposes nothing")) {
        return 6;
    }

    // Automation identity: a realized key maps to the node's UiId; an unrealized key has none.
    if (!expect(semantics.ui_id_of(range.key_at(1)) == kTemplateUiId, "realized key UiId") ||
        !expect(semantics.ui_id_of(range.key_at(10)) == 0, "unrealized key has no UiId") ||
        !expect(semantics.identity_of(range.key_at(10)) == std::nullopt,
                "unrealized key has no identity") ||
        !expect(semantics.identity_of(range.key_at(1)) ==
                    CollectionItemIdentity{kTemplateUiId, range.key_at(1)},
                "realized key identity is (UiId, ItemKey)") ||
        !expect(semantics.identity_of(VirtualItemId{424242}) == std::nullopt,
                "unknown key has no identity")) {
        return 7;
    }
    std::array<char, 24> key_text{};
    if (!expect(CollectionSemantics::format_item_key(range.key_at(2), key_text) == "1014",
                "item key formats as decimal text") ||
        !expect(CollectionSemantics::format_item_key(range.key_at(2),
                                                     std::span<char>(key_text).first(2))
                    .empty(),
                "short key buffer is rejected")) {
        return 8;
    }

    // Active descendant: the realized focused item, tracked across recycle and realize-again.
    const VirtualItemId focused_key = range.key_at(2);
    if (!expect(range.set_focus(focused_key), "focus a realized key")) {
        return 9;
    }
    semantics.observe();
    const std::optional<Announcement> focus_announcement = semantics.take_announcement();
    if (!expect(focus_announcement.has_value(), "focus change stages one announcement") ||
        !expect(focus_announcement->kind == AnnouncementKind::Focus,
                "it is a focus announcement") ||
        !expect(focus_announcement->pos_in_set == 3, "announcement carries the position") ||
        !expect(focus_announcement->set_size == kCount, "announcement carries the set size") ||
        !expect(focus_announcement->realized, "the focused item was realized") ||
        !expect(semantics.active_descendant() == CollectionItemIdentity{kTemplateUiId, focused_key},
                "active descendant is the realized focused item") ||
        !expect(semantics.focused_pos_in_set() == 3, "focused position resolves")) {
        return 10;
    }
    views = {};
    if (!expect(semantics.realized_items(views) == 4, "views after focus") ||
        !expect(views[2].focused && !views[1].focused, "only the focused view is flagged")) {
        return 11;
    }

    // Recycle the focused item: the active descendant disappears, the position does not.
    if (!expect(range.update(500.0f, kViewport).has_value(), "scroll the focused item out")) {
        return 12;
    }
    semantics.observe();
    if (!expect(range.realized_node(2) == nullptr, "index 2 is no longer realized") ||
        !expect(!semantics.active_descendant().has_value(),
                "no active descendant while the focused item is recycled") ||
        !expect(semantics.focused_pos_in_set() == 3, "position survives the recycle") ||
        !expect(!semantics.take_announcement().has_value(),
                "realize/recycle does not re-announce the unchanged focus")) {
        return 13;
    }
    if (!expect(range.update(0.0f, kViewport).has_value(), "scroll the focused item back in")) {
        return 14;
    }
    semantics.observe();
    if (!expect(semantics.active_descendant() == CollectionItemIdentity{kTemplateUiId, focused_key},
                "active descendant is restored when the item is realized again") ||
        !expect(!semantics.take_announcement().has_value(),
                "restoring the realized item does not announce again")) {
        return 15;
    }

    // Selection exposure: data order with positions, realized UiIds only, and a data-order prefix
    // for a short buffer.
    if (!expect(range.set_selected(range.key_at(7), true), "select index 7") ||
        !expect(range.set_selected(range.key_at(2), true), "select index 2") ||
        !expect(range.set_selected(range.key_at(15), true), "select unrealized index 15") ||
        !expect(range.set_selected(range.key_at(4), true), "select index 4")) {
        return 16;
    }
    semantics.observe();
    const std::optional<Announcement> selection_announcement = semantics.take_announcement();
    if (!expect(selection_announcement.has_value(), "selection change stages an announcement") ||
        !expect(selection_announcement->kind == AnnouncementKind::Selection,
                "it is a selection announcement") ||
        !expect(selection_announcement->selected, "it reports the new membership") ||
        !expect(selection_announcement->pos_in_set == 5,
                "it carries the position of the last change")) {
        return 17;
    }
    std::array<SelectionEntry, 8> selection{};
    const std::size_t selected = semantics.selection(selection);
    if (!expect(selected == 4, "four keys selected") ||
        !expect(selection[0].index == 2 && selection[1].index == 4 && selection[2].index == 7 &&
                    selection[3].index == 15,
                "selection is exposed in data order") ||
        !expect(selection[0].pos_in_set == 3 && selection[3].pos_in_set == 16,
                "selection entries carry positions") ||
        !expect(selection[0].identity.ui_id == kTemplateUiId, "realized selection has its UiId") ||
        !expect(selection[3].identity.ui_id == 0, "unrealized selection has no UiId") ||
        !expect(selection[3].identity.item_key == range.key_at(15), "selection carries the key")) {
        return 18;
    }
    std::array<SelectionEntry, 2> short_selection{};
    std::array<char, 40> announcement_text{};
    if (!expect(semantics.selection(short_selection) == 2 && short_selection[0].index == 2 &&
                    short_selection[1].index == 4,
                "short selection buffer keeps data order") ||
        !expect(semantics.selection({}) == 0, "empty selection buffer writes nothing")) {
        return 19;
    }
    const Announcement sample{
        AnnouncementKind::Selection, {kTemplateUiId, range.key_at(4)}, 4, 5, kCount, true, true, 1};
    if (!expect(CollectionSemantics::format_announcement(sample, announcement_text) ==
                    "select item 5 of 20",
                "debug announcement text carries the position") ||
        !expect(CollectionSemantics::format_announcement(
                    Announcement{AnnouncementKind::Focus, {}, 0, 3, kCount, false, false, 2},
                    announcement_text) == "focus item 3 of 20",
                "focus announcement text")) {
        return 20;
    }

    // A short selection toggle removes one key; removing a selected key from the data source drops
    // it from the ordered exposure.
    if (!expect(range.set_selected(range.key_at(7), false), "deselect index 7")) {
        return 21;
    }
    semantics.observe();
    if (!expect(semantics.take_announcement().has_value(), "deselect stages an announcement") ||
        !expect(semantics.selection(selection) == 3 && selection[2].index == 15,
                "deselect removes the entry from data order")) {
        return 22;
    }
    source.keys.erase(source.keys.begin() + 15);
    if (!expect(range.sync().has_value(), "sync after removing a selected key") ||
        !expect(range.item_count() == kCount - 1, "the item count shrank") ||
        !expect(range.update(0.0f, kViewport).has_value(), "realize after the removal")) {
        return 23;
    }
    if (!expect(semantics.set_size() == kCount - 1, "set size follows the removal") ||
        !expect(semantics.selection(selection) == 2, "the removed key is no longer selected") ||
        !expect(semantics.selection(selection) == 2 && selection[0].index == 2 &&
                    selection[0].pos_in_set == 3 && selection[1].index == 4 &&
                    selection[1].pos_in_set == 5,
                "selection order and positions follow the new indices") ||
        !expect(range.clear_selection(), "clear the selection") ||
        !expect(semantics.selection(selection) == 0, "cleared selection exposes nothing")) {
        return 24;
    }

    // An unassigned automation id stays 0: the semantics object never mints an identity.
    RowSource anonymous_source(sparse_keys(4, 9000), 0);
    Node anonymous_container;
    VirtualRange anonymous_range(range_config(), anonymous_source, anonymous_container);
    if (!expect(anonymous_range.sync().has_value() &&
                    anonymous_range.update(0.0f, kViewport).has_value(),
                "sync the source without automation ids")) {
        return 25;
    }
    const CollectionSemantics anonymous(anonymous_range);
    views = {};
    if (!expect(anonymous.realized_items(views) == 4, "all four items realized") ||
        !expect(views[0].identity.ui_id == 0, "no automation id is reported as 0") ||
        !expect(anonymous.ui_id_of(anonymous_range.key_at(0)) == 0, "no automation id on lookup") ||
        !expect(anonymous.identity_of(anonymous_range.key_at(0)) ==
                    CollectionItemIdentity{0, anonymous_range.key_at(0)},
                "identity still carries the key")) {
        return 26;
    }

    // Allocation parity after warm-up: repeated semantics queries and a focus/observe/drain cycle
    // allocate nothing.
    std::array<CollectionItemView, 16> hot_views{};
    std::array<SelectionEntry, 16> hot_selection{};
    (void)range.update(0.0f, kViewport);
    for (std::size_t step = 0; step < 4; ++step) {
        (void)range.set_focus(range.key_at(step));
        semantics.observe();
        while (semantics.take_announcement()) {
        }
        (void)semantics.realized_items(hot_views);
        (void)semantics.selection(hot_selection);
    }
    const std::size_t baseline_allocations = g_allocations;
    bool hot_ok = true;
    for (std::size_t step = 0; step < 200 && hot_ok; ++step) {
        const std::size_t index = step % 4;
        (void)range.set_focus(range.key_at(index));
        semantics.observe();
        while (semantics.take_announcement()) {
        }
        hot_ok = semantics.realized_items(hot_views) == 4;
        hot_ok = hot_ok && semantics.selection(hot_selection) == 0;
        hot_ok = hot_ok && semantics.active_descendant().has_value();
        hot_ok = hot_ok && semantics.pos_in_set(range.key_at(index)).has_value();
        hot_ok = hot_ok && semantics.set_size() == kCount - 1;
    }
    if (!expect(hot_ok, "the hot query loop stays consistent") ||
        !expect(g_allocations == baseline_allocations,
                "semantics queries and announcements allocate nothing after warm-up")) {
        return 27;
    }

    // Determinism: the same script produces identical view, selection, and announcement records.
    const Snapshot first = run_script(60);
    const Snapshot second = run_script(60);
    if (!expect(!first.realized.empty() && first.realized == second.realized,
                "realized views are identical") ||
        !expect(first.selection == second.selection, "selection exposure is identical") ||
        !expect(first.active == second.active && first.focused_pos == second.focused_pos,
                "active descendant is identical") ||
        !expect(first.set_size == second.set_size, "set size is identical") ||
        !expect(first.announcements == second.announcements,
                "announcement sequence is identical")) {
        return 28;
    }

    return 0;
}
