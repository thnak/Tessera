// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/update_scheduler.hpp"

#include <algorithm>
#include <cassert>

namespace tessera::ui {

void UpdateTicket::schedule() {
    if (scheduler_ != nullptr) {
        scheduler_->mark(*this);
    }
}

UpdateScheduler::UpdateScheduler(std::uint32_t max_passes)
    : max_passes_(std::max<std::uint32_t>(max_passes, 1)) {}

void UpdateScheduler::set_wake(WakeFn wake, void* context) {
    wake_ = wake;
    wake_context_ = context;
}

void UpdateScheduler::reserve(std::size_t components) {
    tickets_.reserve(components);
}

bool UpdateScheduler::on_owner_thread() const {
    return std::this_thread::get_id() == owner_;
}

void UpdateScheduler::attach(UpdateTicket& ticket) {
    assert(on_owner_thread() && "UpdateScheduler::attach must run on the UI thread");
    if (ticket.scheduler_ == this) {
        return;
    }
    if (ticket.scheduler_ != nullptr) {
        ticket.scheduler_->detach(ticket);
    }
    ticket.scheduler_ = this;
    ticket.queued_ = false;
    tickets_.push_back(&ticket);
}

void UpdateScheduler::detach(UpdateTicket& ticket) {
    assert(on_owner_thread() && "UpdateScheduler::detach must run on the UI thread");
    if (ticket.scheduler_ != this) {
        return;
    }
    ticket.scheduler_ = nullptr;
    ticket.queued_ = false;
    // Order-preserving erase: attach order is the parent-before-child flush order. A detach during
    // a flush leaves a null hole instead, so the running pass never walks a shifted vector.
    const auto found = std::find(tickets_.begin(), tickets_.end(), &ticket);
    if (found == tickets_.end()) {
        return;
    }
    if (flushing_) {
        *found = nullptr;
    } else {
        tickets_.erase(found);
    }
}

void UpdateScheduler::mark(UpdateTicket& ticket) {
    // Cross-thread state changes must arrive through the completion queue and be applied on the
    // UI thread; failing loudly here beats a silent race on component state.
    assert(on_owner_thread() && "state setter called off the UI thread; post a completion instead");
    if (ticket.queued_) {
        return;
    }
    ticket.queued_ = true;
    const bool edge = !pending_ && !flushing_;
    pending_ = true;
    if (edge && wake_ != nullptr) {
        wake_(wake_context_);
    }
}

bool UpdateScheduler::tick(double dt) {
    assert(on_owner_thread() && "UpdateScheduler::tick must run on the UI thread");
    if (!(dt >= 0.0) || flushing_) {
        return false;
    }
    bool animating = false;
    // Index loop, like flush(): a tick may schedule its owner but never attaches or detaches.
    for (std::size_t i = 0; i < tickets_.size(); ++i) {
        const UpdateTicket* ticket = tickets_[i];
        if (ticket != nullptr && ticket->tick != nullptr) {
            animating = ticket->tick(ticket->context, dt) || animating;
        }
    }
    if (animating && wake_ != nullptr) {
        wake_(wake_context_);
    }
    return animating;
}

UpdateFlushStats UpdateScheduler::flush() {
    assert(on_owner_thread() && "UpdateScheduler::flush must run on the UI thread");
    UpdateFlushStats stats;
    if (!pending_ || flushing_) {
        return stats;
    }
    flushing_ = true;
    while (pending_ && stats.passes < max_passes_) {
        pending_ = false;
        ++stats.passes;
        // Index loop: an update may attach a child (push_back) or detach one (null hole).
        for (std::size_t i = 0; i < tickets_.size(); ++i) {
            UpdateTicket* ticket = tickets_[i];
            if (ticket == nullptr || !ticket->queued_) {
                continue;
            }
            ticket->queued_ = false;
            if (ticket->update != nullptr) {
                ticket->update(ticket->context);
                ++stats.updated;
            }
        }
    }
    std::erase(tickets_, nullptr);
    flushing_ = false;
    if (pending_) {
        // Still dirty after the budget: keep the queued marks for the next flush and ask for
        // another frame so the cycle surfaces in metrics instead of spinning this tick.
        stats.pass_limit_hit = true;
        ++pass_limit_hits_;
        if (wake_ != nullptr) {
            wake_(wake_context_);
        }
    }
    total_.updated += stats.updated;
    total_.passes += stats.passes;
    total_.pass_limit_hit = total_.pass_limit_hit || stats.pass_limit_hit;
    return stats;
}

} // namespace tessera::ui
