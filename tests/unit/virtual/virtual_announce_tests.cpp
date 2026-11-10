// SPDX-License-Identifier: MIT
//
// Coalesced focus/selection announcements for a virtualized collection
// (`docs/specs/virtualization-v1.md` sections 7 and 11, `docs/specs/accessibility-v1.md` section
// 6): exactly one announcement per observed change, a superseded announcement is dropped, an
// unrealized-item focus announces once with its position, observing an unchanged range announces
// nothing, and the path allocates nothing after warm-up.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/collection_semantics.hpp"
#include "tessera/ui/virtual/virtual_range.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <vector>

namespace {

std::size_t g_allocations = 0;

using tessera::ui::Node;
using tessera::ui::automation::UiId;
using tessera::ui::virtualization::Announcement;
using tessera::ui::virtualization::AnnouncementKind;
using tessera::ui::virtualization::CollectionItemIdentity;
using tessera::ui::virtualization::CollectionSemantics;
using tessera::ui::virtualization::kMaxPendingAnnouncements;
using tessera::ui::virtualization::MeasureConstraints;
using tessera::ui::virtualization::VirtualDataSource;
using tessera::ui::virtualization::VirtualItemId;
using tessera::ui::virtualization::VirtualItemMeasure;
using tessera::ui::virtualization::VirtualRange;

constexpr float kMain = 32.0f;
constexpr float kViewport = 100.0f;
constexpr std::uint64_t kBaseKey = 1000;
constexpr UiId kTemplateUiId = 0x51A7'0002ULL;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_announce: " << message << "\n";
    }
    return condition;
}

class RowSource final : public VirtualDataSource {
public:
    explicit RowSource(std::size_t count)
        : count_(count) {}

    [[nodiscard]] std::size_t item_count() const override {
        return count_;
    }
    [[nodiscard]] VirtualItemId item_key(std::size_t index) const override {
        return VirtualItemId{kBaseKey + index * 3};
    }
    [[nodiscard]] VirtualItemMeasure measure(std::size_t,
                                             const MeasureConstraints& constraints) const override {
        return VirtualItemMeasure{kMain, constraints.cross_extent};
    }
    void build(std::size_t, Node& out) override {
        out.set_element_kind("row");
        out.set_automation_id(kTemplateUiId);
    }

private:
    std::size_t count_ = 0;
};

VirtualRange::Config range_config(std::size_t count, std::size_t pool_cap = 16) {
    VirtualRange::Config config;
    config.capacity = count;
    config.pool_cap = pool_cap;
    config.state_capacity = 64;
    config.estimate_main = kMain;
    config.gap = 0.0f;
    config.overscan_main = 0.0f;
    config.cross_extent = 100.0f;
    return config;
}

// Focus and selection changes derived from the step number only, drained after each observation.
std::vector<Announcement> run_script(std::size_t steps) {
    RowSource source(12);
    Node container;
    VirtualRange range(range_config(12), source, container);
    std::vector<Announcement> announcements;
    if (!range.sync().has_value() || !range.update(0.0f, kViewport).has_value()) {
        return announcements;
    }
    CollectionSemantics semantics(range);
    semantics.observe();
    for (std::size_t step = 0; step < steps; ++step) {
        const std::size_t index = (step * 5) % 12;
        switch (step % 5) {
        case 0:
            (void)range.set_focus(range.key_at(index));
            break;
        case 1:
            (void)range.set_selected(range.key_at(index), true);
            break;
        case 2:
            (void)range.set_selected(range.key_at(index), false);
            break;
        case 3:
            (void)range.clear_focus();
            break;
        default:
            (void)range.toggle_selected(range.key_at(index));
            break;
        }
        semantics.observe();
        while (std::optional<Announcement> announcement = semantics.take_announcement()) {
            announcements.push_back(*announcement);
        }
    }
    return announcements;
}

} // namespace

// Count allocations so the announce path can be proven allocation-free after warm-up.
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
    RowSource source(50);
    Node container;
    VirtualRange range(range_config(50), source, container);
    if (!expect(range.sync().has_value(), "sync 50 items") ||
        !expect(range.update(0.0f, kViewport).has_value(), "realize the top window")) {
        return 1;
    }
    CollectionSemantics semantics(range);
    semantics.observe();

    // The baseline observation announces nothing, and neither do repeated observations of an
    // unchanged range: the path is edge-triggered, not per frame.
    if (!expect(!semantics.take_announcement().has_value(), "baseline announces nothing") ||
        !expect(semantics.stats().staged == 0, "no announcement was staged")) {
        return 2;
    }
    for (std::size_t frame = 0; frame < 100; ++frame) {
        semantics.observe();
    }
    if (!expect(!semantics.take_announcement().has_value(), "no per-frame announcement") ||
        !expect(semantics.stats().staged == 0, "observing an unchanged range stages nothing")) {
        return 3;
    }

    // Exactly one announcement per focus change, with the position, and no re-announcement while
    // the focused key stays put (updates, realize/recycle, and frames do not re-announce).
    if (!expect(range.set_focus(range.key_at(1)), "focus index 1")) {
        return 4;
    }
    semantics.observe();
    const std::optional<Announcement> first = semantics.take_announcement();
    if (!expect(first.has_value(), "one announcement for the focus change") ||
        !expect(first->kind == AnnouncementKind::Focus, "focus kind") ||
        !expect(first->identity == CollectionItemIdentity{kTemplateUiId, range.key_at(1)},
                "announcement carries the item identity") ||
        !expect(first->index == 1 && first->pos_in_set == 2,
                "announcement carries index and pos") ||
        !expect(first->set_size == 50, "announcement carries the set size") ||
        !expect(first->realized && !first->selected, "focus announcement flags") ||
        !expect(first->sequence == 1, "first announcement has sequence 1") ||
        !expect(!semantics.take_announcement().has_value(), "exactly one, not two") ||
        !expect(semantics.pending_announcements() == 0, "the drain emptied the ring")) {
        return 5;
    }
    if (!expect(range.update(0.0f, kViewport).has_value(), "an update that keeps the focus") ||
        !expect(range.update(320.0f, kViewport).has_value(), "recycle the focused item") ||
        !expect(range.update(0.0f, kViewport).has_value(), "realize it again")) {
        return 6;
    }
    semantics.observe();
    if (!expect(!semantics.take_announcement().has_value(),
                "recycle/realize does not re-announce the unchanged focus") ||
        !expect(semantics.stats().focus_changes == 1, "only one focus change was observed")) {
        return 7;
    }

    // Coalescing: a burst of changes between two observations keeps exactly one pending
    // announcement, the last state; the superseded one is dropped and counted.
    if (!expect(range.set_focus(range.key_at(2)), "focus index 2")) {
        return 8;
    }
    semantics.observe();
    if (!expect(range.set_focus(range.key_at(3)), "focus index 3 before draining")) {
        return 9;
    }
    semantics.observe();
    if (!expect(semantics.pending_announcements() == 1, "the burst coalesced to one entry") ||
        !expect(semantics.stats().superseded == 1, "the superseded announcement is counted")) {
        return 10;
    }
    const std::optional<Announcement> latest = semantics.take_announcement();
    if (!expect(latest.has_value() && latest->index == 3 && latest->pos_in_set == 4,
                "the surviving announcement is the latest focus") ||
        !expect(latest->sequence == 3, "superseding still advances the sequence") ||
        !expect(!semantics.take_announcement().has_value(), "the superseded entry is gone")) {
        return 11;
    }

    // Focus away and back between observations nets out to no announcement: the observed state is
    // the coalescing key, so the consumer never hears about a position the user already left.
    if (!expect(range.set_focus(range.key_at(1)), "return focus to index 1")) {
        return 12;
    }
    semantics.observe(); // observe the new state so the next step starts from it
    (void)semantics.take_announcement();
    if (!expect(range.set_focus(range.key_at(2)), "focus index 2 briefly") ||
        !expect(range.set_focus(range.key_at(1)), "focus index 1 again")) {
        return 13;
    }
    semantics.observe();
    if (!expect(!semantics.take_announcement().has_value(),
                "a net-unchanged focus announces nothing")) {
        return 14;
    }

    // Clearing focus names no item, so it is not announced, but it is counted as a change.
    const std::size_t changes_before_clear = semantics.stats().focus_changes;
    if (!expect(range.clear_focus(), "clear the focus")) {
        return 15;
    }
    semantics.observe();
    if (!expect(!semantics.take_announcement().has_value(), "clearing focus announces nothing") ||
        !expect(semantics.stats().focus_changes == changes_before_clear + 1,
                "clearing focus is an observed change")) {
        return 16;
    }

    // Selection: one coalesced announcement per observed change, with the last membership state.
    if (!expect(range.set_selected(range.key_at(6), true), "select index 6")) {
        return 17;
    }
    semantics.observe();
    const std::optional<Announcement> selected = semantics.take_announcement();
    if (!expect(selected.has_value() && selected->kind == AnnouncementKind::Selection,
                "selection change stages one announcement") ||
        !expect(selected->selected && selected->pos_in_set == 7, "select position and state") ||
        !expect(selected->identity.item_key == range.key_at(6), "selection carries the key")) {
        return 18;
    }
    if (!expect(range.set_selected(range.key_at(9), true), "select index 9")) {
        return 19;
    }
    semantics.observe();
    if (!expect(range.set_selected(range.key_at(9), false), "deselect index 9 before draining") ||
        !expect(semantics.pending_announcements() == 1, "the selection announcement is pending")) {
        return 19;
    }
    semantics.observe();
    const std::optional<Announcement> coalesced = semantics.take_announcement();
    if (!expect(coalesced.has_value(), "a selection change was announced") ||
        !expect(!coalesced->selected && coalesced->pos_in_set == 10,
                "the last membership change survives coalescing") ||
        !expect(semantics.stats().superseded >= 2, "selection supersession is counted")) {
        return 20;
    }

    // FIFO across kinds: a focus change and a selection change staged before the drain are both
    // exposed, focus first (the order they were observed in).
    if (!expect(range.set_focus(range.key_at(0)), "focus for the FIFO check") ||
        !expect(range.set_selected(range.key_at(11), true), "select for the FIFO check")) {
        return 21;
    }
    semantics.observe();
    if (!expect(semantics.pending_announcements() == 2, "two kinds are pending") ||
        !expect(semantics.pending_announcements() <= kMaxPendingAnnouncements,
                "the ring stays bounded")) {
        return 22;
    }
    const std::optional<Announcement> fifo_focus = semantics.take_announcement();
    const std::optional<Announcement> fifo_selection = semantics.take_announcement();
    if (!expect(fifo_focus.has_value() && fifo_focus->kind == AnnouncementKind::Focus,
                "focus announcement comes first") ||
        !expect(fifo_selection.has_value() && fifo_selection->kind == AnnouncementKind::Selection,
                "selection announcement comes second") ||
        !expect(fifo_selection->identity.item_key == range.key_at(11), "FIFO selection key") ||
        !expect(!semantics.take_announcement().has_value(), "the ring is empty after the drain")) {
        return 23;
    }

    // An unrealized-item focus announces once with the position: a pool-capped window truncates the
    // target, the key stays focused, and realizing it later does not announce again.
    RowSource capped_source(50);
    Node capped_container;
    VirtualRange capped(range_config(50, 2), capped_source, capped_container);
    if (!expect(capped.sync().has_value(), "sync the capped range") ||
        !expect(capped.update(0.0f, kViewport).has_value(), "realize the truncated top window")) {
        return 24;
    }
    CollectionSemantics capped_semantics(capped);
    capped_semantics.observe();
    const auto focus_result = capped.focus_item(capped.key_at(3), kViewport);
    if (!expect(focus_result.has_value(), "focus the truncated target") ||
        !expect(focus_result->truncated && !focus_result->realized,
                "the pool cap keeps the target unrealized") ||
        !expect(capped.focused_node() == nullptr, "no node shows the unrealized focus")) {
        return 25;
    }
    capped_semantics.observe();
    const std::optional<Announcement> unrealized = capped_semantics.take_announcement();
    if (!expect(unrealized.has_value(), "an unrealized focus still announces once") ||
        !expect(unrealized->kind == AnnouncementKind::Focus && !unrealized->realized,
                "the announcement reports the unrealized state") ||
        !expect(unrealized->pos_in_set == 4 && unrealized->set_size == 50,
                "the announcement carries the position") ||
        !expect(unrealized->identity.ui_id == 0, "no UiId is invented for an unrealized item") ||
        !expect(unrealized->identity.item_key == capped.key_at(3), "the key is carried") ||
        !expect(!capped_semantics.take_announcement().has_value(), "exactly one announcement")) {
        return 26;
    }
    if (!expect(capped.update(96.0f, kViewport).has_value(), "scroll the focused item into view") ||
        !expect(capped.realized_node(3) != nullptr, "the focused item is realized now")) {
        return 27;
    }
    capped_semantics.observe();
    if (!expect(!capped_semantics.take_announcement().has_value(),
                "realizing the item does not announce again") ||
        !expect(capped_semantics.active_descendant() ==
                    CollectionItemIdentity{kTemplateUiId, capped.key_at(3)},
                "the realized focus becomes the active descendant")) {
        return 28;
    }

    // Allocation parity after warm-up: focus changes, observation, draining, and the identity
    // lookups allocate nothing.
    for (std::size_t step = 0; step < 8; ++step) {
        (void)range.set_focus(range.key_at(step % 4));
        semantics.observe();
        while (semantics.take_announcement()) {
        }
        (void)semantics.active_descendant();
    }
    const std::size_t baseline_allocations = g_allocations;
    bool hot_ok = true;
    for (std::size_t step = 0; step < 200 && hot_ok; ++step) {
        (void)range.set_focus(range.key_at(step % 4));
        semantics.observe();
        hot_ok = semantics.pending_announcements() == 1;
        hot_ok = hot_ok && semantics.take_announcement().has_value();
        hot_ok = hot_ok && !semantics.take_announcement().has_value();
        hot_ok = hot_ok && semantics.active_descendant().has_value();
        hot_ok = hot_ok && semantics.focused_pos_in_set().has_value();
    }
    if (!expect(hot_ok, "the hot announce loop stays consistent") ||
        !expect(g_allocations == baseline_allocations,
                "focus announcements allocate nothing after warm-up")) {
        return 29;
    }

    // Determinism: the same focus/selection script produces the identical announcement sequence.
    const std::vector<Announcement> first_run = run_script(80);
    const std::vector<Announcement> second_run = run_script(80);
    if (!expect(!first_run.empty(), "the script announced something") ||
        !expect(first_run == second_run, "the announcement sequence is identical")) {
        return 30;
    }

    return 0;
}
