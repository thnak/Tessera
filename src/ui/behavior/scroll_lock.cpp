// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/scroll_lock.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstddef>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `active`.
constexpr std::string_view kOptionNames[] = {"active"};

bool option_bool(const ArgsView& args, std::string_view name, bool fallback) {
    const ArgValue* value = args.find(name, kOptionNames);
    if (value == nullptr || value->kind != ArgValue::Kind::Bool) {
        return fallback;
    }
    return value->boolean;
}

// One node's lock state. `count` is how many behaviors hold a reference; `scrollable_x`/`_y` are
// the axes the node had before the first lock, so releasing the last reference restores exactly
// what was there rather than assuming both were on.
struct LockEntry {
    const Node* node = nullptr;
    std::size_t count = 0;
    bool scrollable_x = false;
    bool scrollable_y = false;
};

// A fixed table, not a map: the number of simultaneously locked nodes is bounded by the number of
// open modal layers, which is a handful, and the lock path must not allocate (ADR-024). A behavior
// that cannot get a slot leaves the node unlocked and reports `locked = false` rather than lying.
constexpr std::size_t kMaxLockedNodes = 16;
std::array<LockEntry, kMaxLockedNodes> g_locks{};

LockEntry* find_entry(const Node& node) {
    for (LockEntry& entry : g_locks) {
        if (entry.node == &node) {
            return &entry;
        }
    }
    return nullptr;
}

LockEntry* free_entry() {
    for (LockEntry& entry : g_locks) {
        if (entry.node == nullptr) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace

ScrollLock::~ScrollLock() {
    // A behavior destroyed without `detach` must not leave a lock behind: the node would stay
    // unscrollable for the rest of the session.
    detach();
}

void ScrollLock::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    (void)services; // a scroll lock reads no interaction state
    if (!targets.all_attached() || targets.nodes.empty()) {
        return;
    }
    target_ = targets.nodes.front();
    apply(option_bool(args, "active", true));
}

void ScrollLock::update(const ArgsView& args) {
    if (target_ == nullptr) {
        return;
    }
    apply(option_bool(args, "active", true));
}

void ScrollLock::detach() {
    if (target_ != nullptr && held_) {
        if (LockEntry* entry = find_entry(*target_); entry != nullptr) {
            if (entry->count > 1) {
                --entry->count;
            } else {
                // The last reference restores the axes the node had before the first lock.
                target_->set_scrollable_x(entry->scrollable_x);
                target_->set_scrollable_y(entry->scrollable_y);
                *entry = LockEntry{};
            }
        }
    }
    target_ = nullptr;
    held_ = false;
    outputs_ = {};
}

std::span<const OutputValue> ScrollLock::outputs() const {
    return outputs_;
}

std::size_t ScrollLock::lock_count(const Node& node) {
    const LockEntry* entry = find_entry(node);
    return entry != nullptr ? entry->count : 0;
}

void ScrollLock::apply(bool active) {
    if (target_ == nullptr) {
        return;
    }
    if (active && !held_) {
        LockEntry* entry = find_entry(*target_);
        if (entry == nullptr) {
            entry = free_entry();
            if (entry == nullptr) {
                // No slot: leave the node unlocked and report the truth.
                outputs_[0].kind = OutputValue::Kind::Bool;
                outputs_[0].boolean = false;
                return;
            }
            *entry = LockEntry{target_, 0, target_->scrollable_x(), target_->scrollable_y()};
        }
        ++entry->count;
        held_ = true;
        target_->set_scrollable_x(false);
        target_->set_scrollable_y(false);
    } else if (!active && held_) {
        if (LockEntry* entry = find_entry(*target_); entry != nullptr) {
            if (entry->count > 1) {
                --entry->count;
            } else {
                target_->set_scrollable_x(entry->scrollable_x);
                target_->set_scrollable_y(entry->scrollable_y);
                *entry = LockEntry{};
            }
        }
        held_ = false;
    }
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean = held_;
}

} // namespace tessera::ui::behavior
