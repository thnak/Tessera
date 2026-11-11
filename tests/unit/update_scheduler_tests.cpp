// SPDX-License-Identifier: MIT
//
// UpdateScheduler (issue #5): batching, parent-before-child order, wake edges, re-entrant dirtying
// with a bounded pass budget, detach during a flush, and the headless no-op while detached.

#include "tessera/ui/scene/update_scheduler.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::ui::UpdateScheduler;
using tessera::ui::UpdateTicket;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "update_scheduler: " << message << "\n";
    }
    return condition;
}

struct Probe {
    UpdateTicket ticket;
    std::vector<std::string>* log = nullptr;
    std::string name;
    int updates = 0;
    // Re-dirty this ticket from its own update this many times.
    int redirty = 0;
    UpdateTicket* also = nullptr;
    UpdateScheduler* detach_from = nullptr;
    UpdateTicket* detach_target = nullptr;

    Probe(std::vector<std::string>& sink, std::string label)
        : log(&sink),
          name(std::move(label)) {
        ticket.context = this;
        ticket.update = [](void* context) {
            auto* self = static_cast<Probe*>(context);
            ++self->updates;
            self->log->push_back(self->name);
            if (self->redirty > 0) {
                --self->redirty;
                self->ticket.schedule();
            }
            if (self->also != nullptr) {
                self->also->schedule();
                self->also = nullptr;
            }
            if (self->detach_from != nullptr) {
                self->detach_from->detach(*self->detach_target);
                self->detach_from = nullptr;
            }
        };
    }
};

struct WakeCounter {
    int wakes = 0;
};

void wake(void* context) {
    ++static_cast<WakeCounter*>(context)->wakes;
}

bool detached_is_noop() {
    std::vector<std::string> log;
    Probe probe(log, "a");
    probe.ticket.schedule();
    return expect(!probe.ticket.queued(), "a detached ticket is never queued") &&
           expect(probe.updates == 0, "a detached ticket never updates");
}

bool batches_and_wakes_once() {
    std::vector<std::string> log;
    UpdateScheduler scheduler;
    WakeCounter counter;
    scheduler.set_wake(&wake, &counter);
    Probe probe(log, "a");
    scheduler.attach(probe.ticket);
    probe.ticket.schedule();
    probe.ticket.schedule();
    probe.ticket.schedule();
    if (!expect(counter.wakes == 1, "three setters wake the loop once (dirty edge)") ||
        !expect(scheduler.pending(), "scheduler is pending") ||
        !expect(probe.updates == 0, "scheduling never runs the update inline")) {
        return false;
    }
    const auto stats = scheduler.flush();
    if (!expect(probe.updates == 1 && stats.updated == 1, "one update for three setters") ||
        !expect(!scheduler.pending(), "flush clears pending") ||
        !expect(scheduler.flush().updated == 0, "an idle flush does nothing")) {
        return false;
    }
    probe.ticket.schedule();
    return expect(counter.wakes == 2, "the next dirty edge wakes again");
}

bool parent_before_child() {
    std::vector<std::string> log;
    UpdateScheduler scheduler;
    Probe parent(log, "parent");
    Probe child(log, "child");
    scheduler.attach(parent.ticket); // mount order
    scheduler.attach(child.ticket);
    child.ticket.schedule();
    parent.ticket.schedule();
    scheduler.flush();
    return expect(log.size() == 2 && log[0] == "parent" && log[1] == "child",
                  "flush follows attach order, not scheduling order");
}

bool reentrant_dirty_within_budget() {
    std::vector<std::string> log;
    UpdateScheduler scheduler(4);
    Probe first(log, "first");
    Probe second(log, "second");
    scheduler.attach(first.ticket);
    scheduler.attach(second.ticket);
    // `second` dirties `first`, which was already flushed this pass: it must run again.
    second.also = &first.ticket;
    second.ticket.schedule();
    const auto stats = scheduler.flush();
    return expect(first.updates == 1 && second.updates == 1,
                  "an earlier ticket dirtied later runs in the next pass") &&
           expect(stats.passes == 2, "two passes") &&
           expect(!stats.pass_limit_hit, "no cycle reported");
}

bool cycle_hits_the_pass_limit() {
    std::vector<std::string> log;
    UpdateScheduler scheduler(3);
    WakeCounter counter;
    scheduler.set_wake(&wake, &counter);
    Probe probe(log, "loop");
    probe.redirty = 100;
    scheduler.attach(probe.ticket);
    probe.ticket.schedule();
    const auto stats = scheduler.flush();
    if (!expect(stats.pass_limit_hit && stats.passes == 3, "the pass budget bounds a cycle") ||
        !expect(probe.updates == 3, "exactly one update per pass") ||
        !expect(scheduler.pending() && probe.ticket.queued(), "the dirty mark survives") ||
        !expect(scheduler.pass_limit_hits() == 1, "the cycle is counted") ||
        !expect(counter.wakes == 2, "a frame is requested to continue")) {
        return false;
    }
    probe.redirty = 0;
    scheduler.flush();
    return expect(!scheduler.pending(), "a later flush drains once the cycle ends");
}

bool detach_during_flush() {
    std::vector<std::string> log;
    UpdateScheduler scheduler;
    Probe parent(log, "parent");
    Probe child(log, "child");
    scheduler.attach(parent.ticket);
    scheduler.attach(child.ticket);
    parent.detach_from = &scheduler;
    parent.detach_target = &child.ticket; // the parent unmounts its child
    parent.ticket.schedule();
    child.ticket.schedule();
    scheduler.flush();
    return expect(child.updates == 0, "a ticket detached mid-flush does not update") &&
           expect(!child.ticket.attached(), "the child is detached") &&
           expect(scheduler.attached_count() == 1, "the registry no longer holds the child");
}

} // namespace

int main() {
    if (!detached_is_noop()) {
        return 1;
    }
    if (!batches_and_wakes_once()) {
        return 2;
    }
    if (!parent_before_child()) {
        return 3;
    }
    if (!reentrant_dirty_within_budget()) {
        return 4;
    }
    if (!cycle_hits_the_pass_limit()) {
        return 5;
    }
    if (!detach_during_flush()) {
        return 6;
    }
    std::cout << "update_scheduler: ok\n";
    return 0;
}
