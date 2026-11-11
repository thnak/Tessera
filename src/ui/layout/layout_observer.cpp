// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_observer.hpp"

#include <algorithm>
#include <cassert>

namespace tessera::ui::layout {
namespace {

std::size_t depth_of(const Node& node) {
    std::size_t depth = 0;
    for (const Node* parent = node.parent(); parent != nullptr; parent = parent->parent()) {
        ++depth;
    }
    return depth;
}

// A node with no layout box (not in the published result) reads as 0x0.
SizeChange observed_size(const LayoutReadModel& model, const Node& node) {
    const std::optional<LayoutInfo> info = model.find(node);
    if (!info) {
        return SizeChange{};
    }
    return SizeChange{info->border_box.width, info->border_box.height, info->content_box.width,
                      info->content_box.height};
}

} // namespace

LayoutObservation::LayoutObservation(Node& node, Fn fn, void* context)
    : node_(&node),
      id_(node.id()),
      fn_(fn),
      context_(context) {}

LayoutObservation::~LayoutObservation() {
    if (set_ != nullptr) {
        set_->detach(*this);
    }
}

LayoutObservation::LayoutObservation(LayoutObservation&& other) noexcept
    : node_(other.node_),
      id_(other.id_),
      fn_(other.fn_),
      context_(other.context_),
      set_(other.set_),
      depth_(other.depth_),
      delivered_(other.delivered_),
      last_(other.last_),
      pending_(other.pending_) {
    if (set_ != nullptr) {
        set_->relocate(&other, this);
    }
    other.set_ = nullptr;
}

LayoutObservation& LayoutObservation::operator=(LayoutObservation&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (set_ != nullptr) {
        set_->detach(*this);
    }
    node_ = other.node_;
    id_ = other.id_;
    fn_ = other.fn_;
    context_ = other.context_;
    set_ = other.set_;
    depth_ = other.depth_;
    delivered_ = other.delivered_;
    last_ = other.last_;
    pending_ = other.pending_;
    if (set_ != nullptr) {
        set_->relocate(&other, this);
    }
    other.set_ = nullptr;
    return *this;
}

LayoutObserverSet::LayoutObserverSet(std::uint32_t max_rounds)
    : max_rounds_(std::clamp<std::uint32_t>(max_rounds, 1, 16)) {}

LayoutObserverSet::~LayoutObserverSet() {
    // An observation must not outlive its set with a dangling back pointer.
    for (LayoutObservation* observation : observations_) {
        if (observation != nullptr) {
            observation->set_ = nullptr;
        }
    }
}

bool LayoutObserverSet::on_owner_thread() const {
    return std::this_thread::get_id() == owner_;
}

void LayoutObserverSet::reserve(std::size_t observations) {
    observations_.reserve(observations);
    due_.reserve(observations);
}

void LayoutObserverSet::attach(LayoutObservation& observation) {
    assert(on_owner_thread() && "LayoutObserverSet::attach must run on the UI thread");
    if (observation.set_ == this) {
        return;
    }
    if (observation.set_ != nullptr) {
        observation.set_->detach(observation);
    }
    observation.set_ = this;
    observation.depth_ = depth_of(*observation.node_);
    observation.delivered_ = false; // the first layout that includes the node delivers
    observations_.push_back(&observation);
}

void LayoutObserverSet::detach(LayoutObservation& observation) {
    assert(on_owner_thread() && "LayoutObserverSet::detach must run on the UI thread");
    if (observation.set_ != this) {
        return;
    }
    observation.set_ = nullptr;
    // The due list holds raw pointers too: a detach from inside a callback must not leave a
    // pointer to an observation that is about to be destroyed.
    std::replace(due_.begin(), due_.end(), &observation, static_cast<LayoutObservation*>(nullptr));
    const auto found = std::find(observations_.begin(), observations_.end(), &observation);
    if (found == observations_.end()) {
        return;
    }
    if (delivering_) {
        *found = nullptr; // tombstone; compacted after the delivery
    } else {
        observations_.erase(found); // attach order is delivery order, so keep it
    }
}

void LayoutObserverSet::relocate(LayoutObservation* from, LayoutObservation* to) {
    std::replace(observations_.begin(), observations_.end(), from, to);
    std::replace(due_.begin(), due_.end(), from, to);
}

std::size_t LayoutObserverSet::collect(const LayoutReadModel& model, std::size_t min_depth) {
    assert(on_owner_thread() && "LayoutObserverSet::collect must run on the UI thread");
    due_.clear();
    deferred_ = 0;
    if (observations_.empty() || model.generation() == 0) {
        return 0; // nothing observes, or no layout has been published yet
    }
    for (LayoutObservation* observation : observations_) {
        if (observation == nullptr) {
            continue;
        }
        assert(observation->node_->id() == observation->id_ &&
               "an observation outlived its node; detach before the node is destroyed");
        const SizeChange current = observed_size(model, *observation->node_);
        if (observation->delivered_ && observation->last_ == current) {
            continue;
        }
        if (observation->depth_ < min_depth) {
            ++deferred_; // stays due: its last delivered size is untouched
            continue;
        }
        observation->pending_ = current;
        due_.push_back(observation);
    }
    return due_.size();
}

LayoutDeliveryStats LayoutObserverSet::deliver() {
    assert(on_owner_thread() && "LayoutObserverSet::deliver must run on the UI thread");
    LayoutDeliveryStats stats;
    delivering_ = true;
    // Index loop: a callback may attach (push_back) or detach (null a slot).
    for (std::size_t i = 0; i < due_.size(); ++i) {
        LayoutObservation* observation = due_[i];
        if (observation == nullptr || observation->set_ != this) {
            continue;
        }
        // Record the delivery before the callback so a callback that detaches or re-dirties sees
        // consistent state.
        observation->last_ = observation->pending_;
        observation->delivered_ = true;
        stats.shallowest_depth = std::min(stats.shallowest_depth, observation->depth_);
        ++stats.delivered;
        if (observation->fn_ != nullptr) {
            observation->fn_(observation->context_, observation->last_);
        }
    }
    delivering_ = false;
    std::erase(observations_, nullptr);
    due_.clear();
    return stats;
}

} // namespace tessera::ui::layout
