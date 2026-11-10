// SPDX-License-Identifier: MIT

// Bounded node recycler (`docs/specs/virtualization-v1.md` section 5): cap enforcement without
// mutation, clean-on-acquire, no item-state leak across recycle, deterministic FIFO reuse, and
// lease validation. Allocation counters prove the hot path stops allocating once the pool is
// warm.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/virtual/node_recycler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>

namespace {

std::size_t g_allocations = 0;

using tessera::ui::Node;
using tessera::ui::virtualization::clear_node_item_state;
using tessera::ui::virtualization::node_carries_item_state;
using tessera::ui::virtualization::NodeRecycler;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "virtual_pool: " << message << "\n";
    }
    return condition;
}

// Gives a pooled node the kind of state a built item carries, without attaching it to a scene.
void dirty_node(Node& node) {
    node.set_element_kind("row");
    node.set_element_id("row-42");
    node.set_class_name("row--selected");
    node.set_role("listitem");
    node.set_automation_id(42);
    node.set_aria("aria-label", "row 42");
    node.set_inline_style("padding", "4px");
    node.set_focusable(true);
    node.set_tab_index(3);
    node.set_z_index(2);
    node.set_visible(true);
    node.set_pseudo_state(tessera::ui::style::PseudoState::Focus);
    node.set_scrollable_y(true);
    node.set_content_size(120.0f, 400.0f);
    node.set_scroll_offset(0.0f, 80.0f);
    node.set_bounds(tessera::rendering::rhi::Rect{0.0f, 0.0f, 120.0f, 48.0f});
    node.set_transform(tessera::rendering::rhi::Mat3::translation(2.0f, 3.0f));
    tessera::ui::PaintData paint;
    paint.text = "row 42";
    node.set_paint_data(std::move(paint));
    node.add_child(std::make_unique<Node>());
}

std::unique_ptr<Node> make_dirty_child() {
    auto child = std::make_unique<Node>();
    child->set_element_kind("cell");
    return child;
}

} // namespace

// Count allocations so the pooled acquire/release path can be proven allocation-free.
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
    // Capacity is a hard floor of one; a zero request does not produce an unusable pool.
    NodeRecycler clamped(0);
    if (!expect(clamped.capacity() == 1, "zero pool cap clamps to one") ||
        !expect(clamped.in_use_count() == 0 && clamped.node_count() == 0,
                "a fresh pool has no nodes")) {
        return 1;
    }

    // Cap enforcement without mutation: the third acquire is refused and changes nothing.
    NodeRecycler pool(2);
    const auto first = pool.acquire();
    const auto second = pool.acquire();
    if (!expect(first.has_value() && second.has_value(), "two slots acquired") ||
        !expect(first->slot == 0 && second->slot == 1, "slots are handed out in creation order") ||
        !expect(first->generation == 1 && second->generation == 1, "first generation") ||
        !expect(pool.node_count() == 2 && pool.in_use_count() == 2, "both nodes in use") ||
        !expect(pool.free_count() == 0, "no free slot while both are out")) {
        return 2;
    }
    const std::size_t allocations_before_denial = pool.stats().allocations;
    if (!expect(!pool.acquire().has_value(), "acquire past the cap is refused") ||
        !expect(pool.stats().denied == 1, "denial is counted") ||
        !expect(pool.node_count() == 2 && pool.in_use_count() == 2 &&
                    pool.stats().allocations == allocations_before_denial,
                "a denied acquire mutates nothing") ||
        !expect(pool.free_count() == 0, "denial does not free a slot")) {
        return 3;
    }
    if (!expect(pool.release(*first), "release frees a slot") ||
        !expect(pool.in_use_count() == 1 && pool.free_count() == 1, "counts after release") ||
        !expect(!pool.release(*first), "a lease can only be released once") ||
        !expect(pool.stats().stale_leases == 1, "double release is a stale lease")) {
        return 4;
    }

    // Clean on acquire: a dirty node is cleared before it is handed out, and `release` requires
    // the component to have cleared it first (the debug assertion in release is the contract).
    NodeRecycler hygiene(2);
    const auto leased = hygiene.acquire();
    if (!expect(leased.has_value(), "hygiene lease")) {
        return 5;
    }
    dirty_node(*leased->node);
    if (!expect(node_carries_item_state(*leased->node), "dirty node reports item state") ||
        !expect(!leased->node->children().empty(), "dirty node has a built child")) {
        return 6;
    }
    clear_node_item_state(*leased->node);
    if (!expect(!node_carries_item_state(*leased->node), "cleared node carries no item state") ||
        !expect(leased->node->children().empty(), "clearing removes built children") ||
        !expect(leased->node->visible() && !leased->node->focusable(),
                "clearing restores interaction defaults") ||
        !expect(leased->node->scroll_y() == 0.0f && !leased->node->scrollable_y(),
                "clearing restores scroll defaults") ||
        !expect(leased->node->dirty() == tessera::ui::Dirty::Clean, "clearing resets dirty work")) {
        return 7;
    }
    if (!expect(hygiene.release(*leased), "clear-then-release is accepted")) {
        return 8;
    }
    const auto reused = hygiene.acquire();
    if (!expect(reused.has_value(), "released node is reacquired") ||
        !expect(reused->node == leased->node && reused->slot == leased->slot,
                "the same node is reused") ||
        !expect(reused->generation == leased->generation + 1,
                "generation invalidates old leases") ||
        !expect(!node_carries_item_state(*reused->node), "reacquired node is clean")) {
        return 9;
    }

    // FIFO reuse: release order determines acquire order, so the pool does not thrash a node that
    // was just fed back and build order is reproducible.
    NodeRecycler fifo(3);
    const auto a = fifo.acquire();
    const auto b = fifo.acquire();
    const auto c = fifo.acquire();
    if (!expect(a && b && c, "three FIFO leases")) {
        return 10;
    }
    const std::array<Node*, 3> original{a->node, b->node, c->node};
    if (!expect(fifo.release(*a) && fifo.release(*b) && fifo.release(*c), "release in order")) {
        return 11;
    }
    for (std::size_t index = 0; index < original.size(); ++index) {
        const auto next = fifo.acquire();
        if (!expect(next.has_value(), "FIFO reacquire") ||
            !expect(next->slot == index && next->node == original[index],
                    "oldest released slot comes back first")) {
            return 12;
        }
    }

    // Scene hand-off: detach transfers ownership, reclaim takes it back; a detached slot still
    // counts against the cap and a wrong-generation lease is rejected.
    NodeRecycler scene_pool(1);
    const auto scene_lease = scene_pool.acquire();
    if (!expect(scene_lease.has_value(), "scene lease")) {
        return 13;
    }
    std::unique_ptr<Node> owned = scene_pool.detach(*scene_lease);
    if (!expect(owned != nullptr && owned.get() == scene_lease->node,
                "detach hands over the node") ||
        !expect(scene_pool.in_use_count() == 1 && scene_pool.node_count() == 1,
                "a detached slot stays in use") ||
        !expect(!scene_pool.acquire().has_value(), "a detached node still consumes the cap") ||
        !expect(!scene_pool.release(*scene_lease), "release is invalid for a detached slot")) {
        return 14;
    }
    // Dirty the detached node as a scene build would, then clean it before reclaiming.
    dirty_node(*owned);
    clear_node_item_state(*owned);
    const auto forged =
        NodeRecycler::Lease{scene_lease->node, scene_lease->slot, scene_lease->generation + 7};
    if (!expect(!scene_pool.reclaim(forged, std::move(owned)), "forged lease is rejected") ||
        !expect(scene_pool.stats().stale_leases >= 2, "stale leases are counted")) {
        return 15;
    }

    // A fresh pool for the reclaim happy path (the forged attempt consumed the node).
    NodeRecycler reclaim_pool(1);
    const auto reclaim_lease = reclaim_pool.acquire();
    if (!expect(reclaim_lease.has_value(), "reclaim lease")) {
        return 16;
    }
    std::unique_ptr<Node> reclaimed_node = reclaim_pool.detach(*reclaim_lease);
    if (!expect(reclaim_pool.reclaim(*reclaim_lease, std::move(reclaimed_node)),
                "reclaim accepts the detached node") ||
        !expect(reclaim_pool.in_use_count() == 0 && reclaim_pool.free_count() == 1,
                "reclaim returns the slot to the free ring") ||
        !expect(reclaim_pool.acquire()->node == reclaim_lease->node, "reclaimed node is reused")) {
        return 17;
    }

    // Determinism: the same script on two pools produces the same slots, generations and stats.
    const auto run_script = [](NodeRecycler& target) {
        std::array<std::uint32_t, 6> slots{};
        std::array<std::uint64_t, 6> generations{};
        std::array<NodeRecycler::Lease, 3> held{};
        for (std::size_t index = 0; index < held.size(); ++index) {
            const auto lease = target.acquire();
            if (!lease) {
                return false;
            }
            held[index] = *lease;
            slots[index] = lease->slot;
            generations[index] = lease->generation;
        }
        for (const NodeRecycler::Lease& lease : held) {
            if (!target.release(lease)) {
                return false;
            }
        }
        for (std::size_t index = 0; index < held.size(); ++index) {
            const auto lease = target.acquire();
            if (!lease) {
                return false;
            }
            slots[3 + index] = lease->slot;
            generations[3 + index] = lease->generation;
            target.release(*lease);
        }
        return slots == std::array<std::uint32_t, 6>{0, 1, 2, 0, 1, 2} &&
               generations == std::array<std::uint64_t, 6>{1, 1, 1, 2, 2, 2};
    };
    NodeRecycler deterministic_a(3);
    NodeRecycler deterministic_b(3);
    if (!expect(run_script(deterministic_a) && run_script(deterministic_b),
                "scripted reuse follows the same order") ||
        !expect(deterministic_a.stats().acquires == deterministic_b.stats().acquires &&
                    deterministic_a.stats().allocations == deterministic_b.stats().allocations &&
                    deterministic_a.stats().pooled_acquires ==
                        deterministic_b.stats().pooled_acquires,
                "two identical scripts report identical stats")) {
        return 18;
    }

    // Hot path: once every slot exists, acquire/release performs no heap allocation.
    NodeRecycler warm(8);
    std::array<NodeRecycler::Lease, 8> warm_leases{};
    for (std::size_t index = 0; index < warm_leases.size(); ++index) {
        const auto lease = warm.acquire();
        if (!expect(lease.has_value(), "warm every slot")) {
            return 19;
        }
        warm_leases[index] = *lease;
    }
    for (const NodeRecycler::Lease& lease : warm_leases) {
        (void)warm.release(lease);
    }
    const std::size_t baseline = g_allocations;
    bool hot_ok = true;
    for (std::size_t step = 0; step < 10000 && hot_ok; ++step) {
        const auto lease = warm.acquire();
        hot_ok = lease.has_value() && !node_carries_item_state(*lease->node);
        if (lease) {
            hot_ok = hot_ok && warm.release(*lease);
        }
    }
    if (!expect(hot_ok, "acquire/release stays valid") ||
        !expect(g_allocations == baseline, "pooled acquire/release performs no allocations") ||
        !expect(warm.stats().pooled_acquires >= 10000,
                "all hot-path acquires came from the pool")) {
        return 20;
    }

    // A dirty child factory keeps the helper used; it is also a reminder that child subtrees are
    // destroyed by the clear path.
    std::unique_ptr<Node> child = make_dirty_child();
    Node parent;
    parent.add_child(std::move(child));
    clear_node_item_state(parent);
    return expect(parent.children().empty(), "clear removes a dirty child subtree") ? 0 : 21;
}
