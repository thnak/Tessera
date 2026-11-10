// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace tessera::ui {
class Node;
} // namespace tessera::ui

namespace tessera::ui::virtualization {

// Recycled item node pool size from `ui.virtualListPoolCap`
// (`docs/specs/buffer-config-v1.md` section 2). The cap bounds every node the collection ever
// creates, so a longer window truncates instead of growing the pool.
inline constexpr std::size_t kDefaultVirtualListPoolCap = 256;
inline constexpr std::size_t kMinVirtualListPoolCap = 1;

// True when a node still carries per-item state: a built subtree, paint data, element metadata,
// aria/inline-style/event bindings, scroll or interaction state, or a scene parent. The recycler
// asserts this is false before pooling a node, and `clear_node_item_state` is what makes it false.
[[nodiscard]] bool node_carries_item_state(const ui::Node& node) noexcept;

// Restores every item-owned field to its Node default and detaches any children. Allocation-free
// unless the node still has children (their destruction is deferred to the caller's allocator);
// this is the "clean on acquire" reset the recycler runs.
void clear_node_item_state(ui::Node& node) noexcept;

// Bounded LIFO-free (FIFO) recycler for virtualized item nodes (`docs/specs/virtualization-v1.md`
// section 5).
//
// Storage is allocated once in the constructor: `pool_cap` slots plus a free-slot ring, so
// acquire/release never allocate once every slot exists. Deterministic order rules:
//   - `acquire` pops the oldest freed slot (the free ring is FIFO), so a node that was just fed
//     back is not immediately handed out again and release/build order is reproducible;
//   - `release`/`reclaim` append to the ring tail, and children of a released node are removed in
//     reverse child order;
//   - slot ids and generation counters only move forward; no pointer or hash decides order.
//
// Ownership: a slot owns its node while the lease is `Owned`. `detach` moves the node to the scene
// (the caller attaches it to a container); `reclaim` takes it back. A stale or forged lease is
// rejected instead of corrupting the ring. The pool is capped: once every slot is in use, acquire
// returns `std::nullopt` and changes nothing (denials are counted, not retried).
class NodeRecycler {
public:
    static constexpr std::uint32_t kInvalidSlot = 0xFFFF'FFFFu;

    // Identifies one checked-out node. `generation` invalidates the handle after the slot is
    // reused, so a late `release`/`reclaim` cannot free a node that now belongs to another item.
    struct Lease {
        ui::Node* node = nullptr;
        std::uint32_t slot = kInvalidSlot;
        std::uint64_t generation = 0;

        [[nodiscard]] explicit operator bool() const noexcept {
            return node != nullptr;
        }
    };

    struct Stats {
        std::size_t acquires = 0;
        std::size_t pooled_acquires = 0; // served from the free ring
        std::size_t allocations = 0;     // Node constructions; warm-up only
        std::size_t releases = 0;        // release (pool-owned)
        std::size_t reclaims = 0;        // reclaim (scene -> pool)
        std::size_t detaches = 0;
        std::size_t denied = 0;           // acquire with every slot in use
        std::size_t stale_leases = 0;     // release/reclaim/detach with a stale or wrong lease
        std::size_t clean_violations = 0; // debug: pooled node still carried item state
    };

    // `pool_cap` is clamped to at least kMinVirtualListPoolCap.
    explicit NodeRecycler(std::size_t pool_cap);
    ~NodeRecycler();

    NodeRecycler(const NodeRecycler&) = delete;
    NodeRecycler& operator=(const NodeRecycler&) = delete;

    // Returns a clean node, or nullopt when every slot is in use. A pooled node has been cleared;
    // a newly created node starts clean by construction. Allocates only while the pool is still
    // being filled, so callers that warm the cap never see an allocation here.
    [[nodiscard]] std::optional<Lease> acquire();
    // Hands the owned node to the caller (scene attach) while keeping the slot in use.
    [[nodiscard]] std::unique_ptr<ui::Node> detach(Lease lease) noexcept;
    // Pools a node the recycler still owns. False for a stale lease or a detached slot.
    bool release(Lease lease) noexcept;
    // Pools a node the caller detached and now hands back (scene remove). `owned` must be the
    // leased node; a mismatch is rejected and the node is destroyed with the argument.
    bool reclaim(Lease lease, std::unique_ptr<ui::Node> owned) noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] std::size_t in_use_count() const noexcept {
        return in_use_;
    }
    [[nodiscard]] std::size_t free_count() const noexcept {
        return free_count_;
    }
    // Slots that exist (allocated or detached); never exceeds capacity().
    [[nodiscard]] std::size_t node_count() const noexcept {
        return created_;
    }
    [[nodiscard]] Stats stats() const noexcept {
        return stats_;
    }

private:
    enum class SlotState : std::uint8_t {
        Free,
        Owned,
        Detached,
    };

    struct Slot {
        std::unique_ptr<ui::Node> node;
        ui::Node* leased = nullptr; // lease target while Owned/Detached; null while Free
        std::uint64_t generation = 0;
        SlotState state = SlotState::Free;
    };

    [[nodiscard]] bool matches(Lease lease) const noexcept;
    void push_free(std::uint32_t slot) noexcept;
    void note_pooled(ui::Node& node) noexcept;

    std::size_t capacity_ = 0;
    std::size_t created_ = 0;
    std::size_t in_use_ = 0;
    std::size_t free_head_ = 0;
    std::size_t free_count_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_ring_;
    Stats stats_;
};

} // namespace tessera::ui::virtualization
