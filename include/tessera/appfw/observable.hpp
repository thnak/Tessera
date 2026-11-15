// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/update_scheduler.hpp"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace tessera::appfw {

// An app-scoped value whose observers hear about a change once per scheduler flush instead of once
// per write (ADR-050 section 6, issue #176).
//
// Coalescing is the whole point: a hover handler that runs on every mouse move may call `set()`
// hundreds of times between two frames. Each call is a compare, an assignment and one dirty bit on
// this observable's own `UpdateTicket`, so the frame after them sees one notification, and a tick
// with no `set()` sees none.
//
// The observable owns that ticket rather than borrowing a component's, so "N sets -> exactly one
// notification" is a property of the observable itself and an observer can be a plain callback. The
// ticket is a member and the type is non-movable, so the address `UpdateScheduler::attach` stored
// stays valid for the observable's whole life. `subscribe(UpdateTicket&)` is the form ADR-050
// section 6 describes (an observer that is another component's update); it coalesces in the same
// scheduler through the same dirty bit.
//
// Delivery model: `const T&`, bound to the live value, never a copy per observer. Observers run
// synchronously on the UI thread inside the flush, so a copy of a large `T` would be paid on the
// hot path to hand out a value the frame is about to read anyway. Two consequences are part of the
// contract: a callback may not keep the reference after it returns, and a callback that calls
// `set()` changes the value immediately (observers still to run in that pass see the new value)
// while the scheduler's pass loop delivers another notification, bounded by
// `UpdateScheduler::kDefaultMaxPasses`.
//
// Allocation: `set()` allocates nothing and neither does a notification pass. Subscribing allocates
// one node (plus the observer vector's storage) and constructing an observable attaches a ticket
// (the scheduler's ticket vector); all of that is mount/start work.
// `tests/unit/appfw_observable_allocation.cpp` counts every allocation in the process to hold that
// line.
//
// Unsubscribe semantics: immediate, with deferred reclamation. An observer removed during a
// notification (by itself or by another one) is unlinked at once and is not called again in that
// pass or any later one. Its slot is nulled rather than erased while a pass is running, so
// iteration never walks shifted storage, and the holes are dropped when the pass ends: the same
// shape `UpdateScheduler::detach` already uses for a ticket detached mid-flush, for the same
// reason. The unlink is linear in the registry size and moves no elements, so a callback can
// unsubscribe without a `memmove` on the frame path.
template<typename T> class Observable {
    static_assert(
        std::equality_comparable<T>,
        "Observable<T> compares values with == to coalesce writes; a type without == cannot skip a "
        "redundant set()");
    static_assert(
        std::copy_constructible<T> && std::is_copy_assignable_v<T>,
        "Observable<T> stores and assigns the value; set(const T&) needs copy assignment");

public:
    // Observers are C functions with an explicit context: no `std::function`, so notifying never
    // type-erases a closure and never allocates.
    using Callback = void (*)(void* context, const T& value);

private:
    struct ObserverNode {
        Observable* owner = nullptr;
        Callback callback = nullptr;
        void* context = nullptr;

        // Unlinks from the owning observable when it is still alive, then leaves the node for
        // `Subscription` to delete.
        void release() noexcept;
    };

public:
    // RAII link to one observable, move-only, unsubscribes on destruction. It is what a component
    // or service keeps per subscription and releases in `unmount()`/`stop()`. A subscription may
    // outlive its observable: the observable detaches its nodes when it dies, so the release is
    // then a no-op instead of a dangling unlink.
    class Subscription {
    public:
        Subscription() noexcept = default;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        Subscription(Subscription&& other) noexcept
            : node_(other.node_) {
            other.node_ = nullptr;
        }

        Subscription& operator=(Subscription&& other) noexcept {
            if (this != &other) {
                unsubscribe();
                node_ = other.node_;
                other.node_ = nullptr;
            }
            return *this;
        }

        ~Subscription() {
            unsubscribe();
        }

        // Idempotent, and safe from inside a notification, including on the observer itself.
        void unsubscribe() noexcept {
            if (node_ != nullptr) {
                node_->release();
                delete node_;
                node_ = nullptr;
            }
        }

        [[nodiscard]] bool active() const noexcept {
            return node_ != nullptr;
        }

    private:
        friend class Observable;

        explicit Subscription(ObserverNode* node) noexcept
            : node_(node) {}

        ObserverNode* node_ = nullptr;
    };

    // `scheduler` is the window's (or a headless test's) UpdateScheduler; `initial` is the value an
    // observer reads until the first change. Attaching the ticket is the one observable-level
    // allocation, and it happens here, at mount, not on `set()`.
    Observable(ui::UpdateScheduler& scheduler, T initial)
        : scheduler_(&scheduler),
          value_(std::move(initial)) {
        ticket_.update = &Observable::on_ticket_update;
        ticket_.context = this;
        scheduler.attach(ticket_);
    }

    // Non-movable: `ticket_` must keep the address it had at `attach` (UpdateTicket's own rule).
    Observable(const Observable&) = delete;
    Observable& operator=(const Observable&) = delete;
    Observable(Observable&&) = delete;
    Observable& operator=(Observable&&) = delete;

    ~Observable() {
        // The nodes are owned by their Subscriptions, so this only breaks the back-pointers: a
        // subscription released after the observable is then a safe no-op rather than a dangling
        // unlink, and a subscription that never is released still frees its own node.
        for (ObserverNode* node : nodes_) {
            if (node != nullptr) {
                node->owner = nullptr;
            }
        }
        nodes_.clear();
        scheduler_->detach(ticket_);
    }

    [[nodiscard]] const T& get() const noexcept {
        return value_;
    }

    // Monotonic count of accepted changes.
    [[nodiscard]] std::uint64_t version() const noexcept {
        return version_;
    }

    // True while a notification is scheduled for this observable. `set()` only raises this; it
    // never notifies inline.
    [[nodiscard]] bool dirty() const noexcept {
        return ticket_.queued();
    }

    // Compares, stores, raises the dirty bit. Returns true when the value actually changed. No
    // allocation on either path, and an unchanged value does not dirty the scheduler at all, so a
    // handler that re-asserts the same value costs nothing.
    bool set(const T& next) {
        if (value_ == next) {
            return false;
        }
        value_ = next;
        ++version_;
        ticket_.schedule();
        return true;
    }

    bool set(T&& next) {
        if (value_ == next) {
            return false;
        }
        value_ = std::move(next);
        ++version_;
        ticket_.schedule();
        return true;
    }

    // Registers `callback` to run once per flush in which the value changed. Allocates one node
    // (plus vector growth), never on `set()`.
    [[nodiscard]] Subscription subscribe(Callback callback, void* context) {
        assert(callback != nullptr && "an observer needs a callback");
        // Held in a unique_ptr until the vector accepted it, so a throw from the vector's growth
        // cannot leak the node (subscribing is mount work, but it must still be exception-clean).
        auto node = std::unique_ptr<ObserverNode>(new ObserverNode{this, callback, context});
        nodes_.push_back(node.get());
        return Subscription(node.release());
    }

    // ADR-050 section 6 form: the observer is another component's ticket, so that component's
    // `update()` runs at the flush. Many `set()` calls mark the ticket once, so the component still
    // updates once per tick.
    [[nodiscard]] Subscription subscribe(ui::UpdateTicket& ticket) {
        return subscribe(&Observable::schedule_ticket, &ticket);
    }

    [[nodiscard]] std::size_t observer_count() const noexcept {
        return nodes_.size();
    }

private:
    static void on_ticket_update(void* context) {
        static_cast<Observable*>(context)->notify();
    }

    static void schedule_ticket(void* context, const T& /*value*/) {
        static_cast<ui::UpdateTicket*>(context)->schedule();
    }

    void notify() {
        notifying_ = true;
        // Snapshot the size: an observer subscribed from a callback subscribed after this change,
        // so it is not called for it. Indices stay valid across a push_back because the vector is a
        // member and only its storage may move.
        const std::size_t count = nodes_.size();
        try {
            for (std::size_t index = 0; index < count; ++index) {
                const ObserverNode* node = nodes_[index];
                if (node == nullptr) {
                    continue; // unsubscribed earlier in this pass
                }
                // Read the fields first: the callback may unsubscribe itself, deleting the node.
                const Callback callback = node->callback;
                void* const context = node->context;
                callback(context, value_);
            }
        } catch (...) {
            // Observers are user code; a throw must not leave the observable mid-pass. The
            // exception still propagates to the caller's boundary (ADR-025).
            notifying_ = false;
            compact();
            throw;
        }
        notifying_ = false;
        compact();
    }

    void remove(ObserverNode* node) noexcept {
        if (node == nullptr) {
            return;
        }
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            if (nodes_[index] != node) {
                continue;
            }
            if (notifying_) {
                // Deferred reclamation: null the slot so the running pass skips it without walking
                // shifted storage; notify() compacts when the pass ends.
                nodes_[index] = nullptr;
            } else {
                nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(index));
            }
            break;
        }
    }

    void compact() noexcept {
        std::size_t write = 0;
        for (std::size_t read = 0; read < nodes_.size(); ++read) {
            if (nodes_[read] != nullptr) {
                nodes_[write++] = nodes_[read];
            }
        }
        nodes_.resize(write);
    }

    ui::UpdateScheduler* scheduler_ = nullptr;
    ui::UpdateTicket ticket_{};
    T value_;
    std::vector<ObserverNode*> nodes_;
    std::uint64_t version_ = 0;
    bool notifying_ = false;
};

template<typename T> void Observable<T>::ObserverNode::release() noexcept {
    if (owner != nullptr) {
        owner->remove(this);
        owner = nullptr;
    }
}

} // namespace tessera::appfw
