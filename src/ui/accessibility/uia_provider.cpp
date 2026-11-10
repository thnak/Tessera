// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_provider.hpp"

#include <algorithm>
#include <utility>

namespace tessera::ui::a11y {

UiaProvider::UiaProvider() {
    snapshot_.nodes.reserve(kMaxNodes);
    parents_.fill(kInvalidIndex);
}

void UiaProvider::set_snapshot(A11ySnapshot& snapshot) {
    if (snapshot.nodes.size() > kMaxNodes) {
        snapshot.nodes.resize(kMaxNodes);
    }
    snapshot_.nodes.swap(snapshot.nodes);
    rebuild_parents();
    ++generation_;
}

void UiaProvider::adopt_thread(std::thread::id thread) {
    thread_ = thread;
}

bool UiaProvider::owns_thread() const {
    return thread_ == std::this_thread::get_id();
}

std::expected<void, UiaError> UiaProvider::check_thread() const {
    if (thread_ == std::thread::id{} || owns_thread()) {
        return {};
    }
    return std::unexpected(UiaError::ThreadViolation);
}

bool UiaProvider::has_snapshot() const {
    return !snapshot_.nodes.empty();
}

std::size_t UiaProvider::node_count() const {
    return snapshot_.nodes.size();
}

A11yIndex UiaProvider::root() const {
    return snapshot_.nodes.empty() ? kInvalidIndex : 0;
}

A11yIndex UiaProvider::parent_of(A11yIndex index) const {
    if (static_cast<std::size_t>(index) >= snapshot_.nodes.size()) {
        return kInvalidIndex;
    }
    return parents_[index];
}

std::uint16_t UiaProvider::index_in_parent(A11yIndex index) const {
    const A11yIndex parent = parent_of(index);
    if (parent == kInvalidIndex) {
        return 0;
    }
    const A11yNode& node = snapshot_.nodes[parent];
    for (std::uint16_t child = 0; child < node.child_count; ++child) {
        if (node.children[child] == index) {
            return child;
        }
    }
    return 0;
}

const A11yNode* UiaProvider::node(A11yIndex index) const {
    if (static_cast<std::size_t>(index) >= snapshot_.nodes.size()) {
        return nullptr;
    }
    return &snapshot_.nodes[index];
}

const A11ySnapshot& UiaProvider::snapshot() const {
    return snapshot_;
}

std::uint64_t UiaProvider::generation() const {
    return generation_;
}

std::expected<void, UiaError> UiaProvider::request_action(A11yIndex index, Action action,
                                                          double value) {
    if (const auto thread = check_thread(); !thread) {
        return thread;
    }
    const A11yNode* target = node(index);
    if (target == nullptr) {
        return std::unexpected(UiaError::UnknownNode);
    }
    if (action == Action::None) {
        return std::unexpected(UiaError::PatternNotSupported);
    }
    if (has_state(target->states, State::Disabled)) {
        return std::unexpected(UiaError::ElementNotEnabled);
    }
    if (!has_action(target->actions, action)) {
        return std::unexpected(UiaError::PatternNotSupported);
    }
    const UiaActionRequest request{index, action, value};
    {
        std::lock_guard lock(actions_mutex_);
        if (!queue_locked(request)) {
            return std::unexpected(UiaError::QueueFull);
        }
    }
    return {};
}

std::size_t UiaProvider::take_actions(std::span<UiaActionRequest> out) {
    std::lock_guard lock(actions_mutex_);
    const std::size_t taken = std::min(out.size(), action_count_);
    for (std::size_t offset = 0; offset < taken; ++offset) {
        out[offset] = actions_[(action_head_ + offset) % kMaxPendingUiaActions];
    }
    action_head_ = (action_head_ + taken) % kMaxPendingUiaActions;
    action_count_ -= taken;
    return taken;
}

std::size_t UiaProvider::queued_actions() const {
    std::lock_guard lock(actions_mutex_);
    return action_count_;
}

std::uint64_t UiaProvider::coalesced_actions() const {
    std::lock_guard lock(actions_mutex_);
    return coalesced_;
}

void UiaProvider::rebuild_parents() {
    parents_.fill(kInvalidIndex);
    for (std::size_t index = 0; index < snapshot_.nodes.size(); ++index) {
        const A11yNode& node = snapshot_.nodes[index];
        for (std::uint16_t child = 0; child < node.child_count; ++child) {
            const A11yIndex target = node.children[child];
            if (static_cast<std::size_t>(target) < snapshot_.nodes.size()) {
                parents_[target] = static_cast<A11yIndex>(index);
            }
        }
    }
}

bool UiaProvider::queue_locked(const UiaActionRequest& request) {
    // A client can invoke the same node repeatedly before the UI thread drains; one final request
    // is enough, and SetValue keeps the newest payload.
    for (std::size_t offset = 0; offset < action_count_; ++offset) {
        UiaActionRequest& queued = actions_[(action_head_ + offset) % kMaxPendingUiaActions];
        if (queued.node == request.node && queued.action == request.action) {
            queued.value = request.value;
            ++coalesced_;
            return true;
        }
    }
    if (action_count_ == kMaxPendingUiaActions) {
        return false;
    }
    actions_[(action_head_ + action_count_) % kMaxPendingUiaActions] = request;
    ++action_count_;
    return true;
}

} // namespace tessera::ui::a11y
