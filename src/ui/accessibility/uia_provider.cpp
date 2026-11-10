// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_provider.hpp"

#include <algorithm>

namespace tessera::ui::a11y {
namespace {

// Snapshot text arrays are always NUL-terminated (the builder truncates at a code-point boundary),
// so a string_view constructor is the bounded view.
void copy_text(std::array<char, kMaxTextBytes>& target, std::string_view text) {
    const std::size_t size = std::min(text.size(), kMaxTextBytes - 1U);
    std::copy_n(text.begin(), size, target.begin());
    target[size] = '\0';
}

} // namespace

std::string_view text_of(const UiaActionRequest& request) {
    return std::string_view(request.text.data());
}

std::string_view name_of(const UiaElementInfo& element) {
    return std::string_view(element.name.data());
}

std::string_view value_of(const UiaElementInfo& element) {
    return std::string_view(element.value.data());
}

std::string_view description_of(const UiaElementInfo& element) {
    return std::string_view(element.description.data());
}

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

std::expected<UiaElementInfo, UiaError> UiaProvider::element(A11yIndex index) const {
    if (const auto owned = check_thread(); !owned) {
        return std::unexpected(owned.error());
    }
    if (snapshot_.nodes.empty()) {
        return std::unexpected(UiaError::NoSnapshot);
    }
    const A11yNode* source = node(index);
    if (source == nullptr) {
        return std::unexpected(UiaError::UnknownNode);
    }
    UiaElementInfo info;
    info.index = index;
    info.parent = parent_of(index);
    info.control_type = control_type_for(source->role);
    info.patterns = patterns_for(*source);
    info.name = source->name;
    info.value = source->value;
    info.description = source->description;
    info.bounds = source->bounds;
    info.automation_id = source->automation_id;
    info.index_in_parent = index_in_parent(index);
    info.enabled = !has_state(source->states, State::Disabled);
    info.focused = has_state(source->states, State::Focused);
    info.selected = has_state(source->states, State::Selected);
    return info;
}

std::expected<std::uint16_t, UiaError> UiaProvider::child_count(A11yIndex index) const {
    if (const auto owned = check_thread(); !owned) {
        return std::unexpected(owned.error());
    }
    const A11yNode* source = node(index);
    if (source == nullptr) {
        return std::unexpected(snapshot_.nodes.empty() ? UiaError::NoSnapshot
                                                       : UiaError::UnknownNode);
    }
    return source->child_count;
}

std::expected<A11yIndex, UiaError> UiaProvider::child_at(A11yIndex index,
                                                         std::uint16_t child) const {
    if (const auto owned = check_thread(); !owned) {
        return std::unexpected(owned.error());
    }
    const A11yNode* source = node(index);
    if (source == nullptr) {
        return std::unexpected(snapshot_.nodes.empty() ? UiaError::NoSnapshot
                                                       : UiaError::UnknownNode);
    }
    if (child >= source->child_count) {
        return std::unexpected(UiaError::UnknownNode);
    }
    return source->children[child];
}

std::expected<A11yIndex, UiaError> UiaProvider::focused() const {
    if (const auto owned = check_thread(); !owned) {
        return std::unexpected(owned.error());
    }
    if (snapshot_.nodes.empty()) {
        return std::unexpected(UiaError::NoSnapshot);
    }
    for (std::size_t index = 0; index < snapshot_.nodes.size(); ++index) {
        if (has_state(snapshot_.nodes[index].states, State::Focused)) {
            return static_cast<A11yIndex>(index);
        }
    }
    return root();
}

std::expected<A11yIndex, UiaError> UiaProvider::hit_test(double x, double y) const {
    if (const auto owned = check_thread(); !owned) {
        return std::unexpected(owned.error());
    }
    if (snapshot_.nodes.empty()) {
        return std::unexpected(UiaError::NoSnapshot);
    }
    // Reverse pre-order: later entries are deeper or later siblings, hence painted on top.
    for (std::size_t offset = snapshot_.nodes.size(); offset-- > 0;) {
        const A11yNode& candidate = snapshot_.nodes[offset];
        const rendering::rhi::Rect& bounds = candidate.bounds;
        if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
            continue;
        }
        const double left = static_cast<double>(bounds.x);
        const double top = static_cast<double>(bounds.y);
        const double right = static_cast<double>(bounds.right());
        const double bottom = static_cast<double>(bounds.bottom());
        if (x >= left && x < right && y >= top && y < bottom) {
            return static_cast<A11yIndex>(offset);
        }
    }
    return root();
}

std::expected<void, UiaError> UiaProvider::request_action(A11yIndex index, Action action,
                                                          double value, std::string_view text) {
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
    UiaActionRequest request;
    request.node = index;
    request.action = action;
    request.value = value;
    copy_text(request.text, text);
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
            queued.text = request.text;
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
