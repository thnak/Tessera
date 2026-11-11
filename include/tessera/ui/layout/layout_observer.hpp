// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_read_model.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace tessera::ui::layout {

// What an observation delivers: the border box and the content box size, in logical pixels
// (docs/design/layout-observation.md section 5.5). A node with no layout box reads as zeros.
struct SizeChange {
    float width = 0.0f;
    float height = 0.0f;
    float content_width = 0.0f;
    float content_height = 0.0f;

    bool operator==(const SizeChange&) const = default;
};

class LayoutObserverSet;

// One size observation of one node. Attach it to the window's `LayoutObserverSet`; it detaches in
// its destructor, so a mounted component owns it like any other member. Move-only: the set keeps a
// pointer to the live object and follows a move. The node must outlive the attachment.
class LayoutObservation {
public:
    using Fn = void (*)(void* context, const SizeChange& size);

    LayoutObservation(Node& node, Fn fn, void* context);
    ~LayoutObservation();

    LayoutObservation(LayoutObservation&& other) noexcept;
    LayoutObservation& operator=(LayoutObservation&& other) noexcept;
    LayoutObservation(const LayoutObservation&) = delete;
    LayoutObservation& operator=(const LayoutObservation&) = delete;

    [[nodiscard]] bool attached() const {
        return set_ != nullptr;
    }
    [[nodiscard]] const Node& node() const {
        return *node_;
    }

private:
    friend class LayoutObserverSet;

    Node* node_ = nullptr;
    NodeId id_ = 0;
    Fn fn_ = nullptr;
    void* context_ = nullptr;
    LayoutObserverSet* set_ = nullptr;
    std::size_t depth_ = 0;  // distance from the scene root at attach time
    bool delivered_ = false; // false until the first delivery (the initial size)
    SizeChange last_;        // last delivered size
    SizeChange pending_;     // size chosen by the last collect
};

struct LayoutDeliveryStats {
    std::uint32_t delivered = 0;
    // Depth of the shallowest observation delivered; kNoDepth when nothing was delivered. The
    // settle loop raises the next round's depth floor above it (slice 3).
    std::size_t shallowest_depth = std::numeric_limits<std::size_t>::max();
};

inline constexpr std::size_t kNoDepth = std::numeric_limits<std::size_t>::max();

// The notification side of layout (the read model is the query side): one per window. After each
// layout the host collects the observations whose size changed since their last delivery and
// delivers them, at most once per call, in attach (mount) order, so parent before child and the
// same order every run.
//
// Steady-state cost: with nothing attached `collect` returns after one size check; with k
// observations it is O(k) per call and allocation-free after `reserve`. Everything is UI-thread
// only (asserted in debug builds). Callbacks may call setters (they only mark a ticket) and may
// detach observations; an observation attached from a callback is first collected by the next
// `collect`. This type implements one collect/deliver step; the settle rounds that loop it with
// the update scheduler are a separate host helper.
class LayoutObserverSet {
public:
    // Round budget for the settle loop; stored here so the host reads one configuration object.
    explicit LayoutObserverSet(std::uint32_t max_rounds = 4);
    ~LayoutObserverSet();

    LayoutObserverSet(const LayoutObserverSet&) = delete;
    LayoutObserverSet& operator=(const LayoutObserverSet&) = delete;

    void reserve(std::size_t observations);

    // Registers `observation` (idempotent); attach order is delivery order. Detach is safe during
    // delivery: the entry is tombstoned and compacted afterwards.
    void attach(LayoutObservation& observation);
    void detach(LayoutObservation& observation);

    // Selects the observations due for delivery from the published layout: the observed size
    // differs from the last delivered one (or nothing was delivered yet), and the observation is
    // at least `min_depth` deep. Changed observations shallower than `min_depth` stay due and are
    // counted in `deferred()`. Nothing is selected before the first layout was published.
    // Returns the number selected.
    std::size_t collect(const LayoutReadModel& model, std::size_t min_depth = 0);

    // Calls every selected observation once, in attach order, with its latest size.
    LayoutDeliveryStats deliver();

    [[nodiscard]] std::size_t size() const {
        return observations_.size();
    }
    [[nodiscard]] std::size_t deferred() const {
        return deferred_;
    }
    [[nodiscard]] std::uint32_t max_rounds() const {
        return max_rounds_;
    }

private:
    friend class LayoutObservation;

    void relocate(LayoutObservation* from, LayoutObservation* to);
    [[nodiscard]] bool on_owner_thread() const;

    std::vector<LayoutObservation*> observations_; // attach order; nullptr is a tombstone
    std::vector<LayoutObservation*> due_;
    std::size_t deferred_ = 0;
    std::uint32_t max_rounds_ = 4;
    bool delivering_ = false;
    std::thread::id owner_ = std::this_thread::get_id();
};

} // namespace tessera::ui::layout
