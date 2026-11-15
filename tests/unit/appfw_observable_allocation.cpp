// SPDX-License-Identifier: MIT
//
// Allocation proof for `Observable<T>::set()` (issue #176, ADR-050 section 6): the mandatory
// allocation-counting test. It replaces the global `operator new`/`new[]`/`delete` pair exactly
// like `tests/unit/chart_series_ring_allocation.cpp` (which follows
// `tests/benchmarks/stylesheet/benchmark_counters.cpp`), so every allocation in this binary is
// counted: a measured window that reports no change really did not allocate, with no sampling, RSS
// or clock involved.
//
// The contract under test:
//   * `set()` on a live observable allocates zero blocks and zero bytes, whether or not the value
//     changed, and whether or not a flush is pending;
//   * a flush that delivers notifications allocates nothing either, including when an observer
//     unsubscribes during the pass;
//   * subscribing allocates (that is mount work) and releasing a subscription does not.
//
// GCC at -O3 inlines the replaced operator new/delete pair into containers and then flags the
// free() inside our operator delete; the pair is intentionally malloc/free (same as the benchmark).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

#include "appfw_observable_test_support.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace {

std::size_t g_allocation_count = 0;
std::size_t g_allocated_bytes = 0;

struct HeapWindow {
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

[[nodiscard]] HeapWindow heap_window() noexcept {
    return HeapWindow{g_allocation_count, g_allocated_bytes};
}

} // namespace

// Replaced global allocation functions: the only allocation mechanism in this binary, so the
// windows are exact. Deallocations are not counted because the contract is "no allocation on
// set()", not a memory high-water mark.
void* operator new(std::size_t size) {
    ++g_allocation_count;
    g_allocated_bytes += size;
    if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

namespace appfw_test {

namespace {

int observer_calls = 0;

void count_observer(void* /*context*/, const int& /*value*/) {
    ++observer_calls;
}

struct UpdateProbe {
    ui::UpdateTicket ticket;
    int updates = 0;
};

// The acceptance test: a live observable with live observers, then thousands of `set()` calls and
// the flush that coalesces them, all inside measured windows.
bool set_and_flush_allocate_nothing() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    auto callback_subscription = observable.subscribe(&count_observer, nullptr);

    UpdateProbe probe;
    probe.ticket.context = &probe;
    probe.ticket.update = [](void* context) { ++static_cast<UpdateProbe*>(context)->updates; };
    scheduler.attach(probe.ticket);
    auto ticket_subscription = observable.subscribe(probe.ticket);

    // Setup is done; everything measured from here runs on the live object.
    constexpr int kSets = 10000;
    const HeapWindow before = heap_window();
    for (int i = 1; i <= kSets; ++i) {
        observable.set(i);
    }
    const HeapWindow after_sets = heap_window();
    const ui::UpdateFlushStats stats = scheduler.flush();
    const HeapWindow after_flush = heap_window();

    // A value that does not change must also stay off the heap (and off the dirty queue).
    for (int i = 0; i < 1000; ++i) {
        observable.set(kSets);
    }
    const HeapWindow after_noops = heap_window();

    // 1000 more ticks after the coalesced flush: nothing changed, so nothing runs.
    for (int tick = 0; tick < 1000; ++tick) {
        scheduler.flush();
    }
    const HeapWindow after_idle_ticks = heap_window();

    const std::size_t set_allocations = after_sets.allocations - before.allocations;
    const std::size_t flush_allocations = after_flush.allocations - after_sets.allocations;
    const std::size_t noop_allocations = after_noops.allocations - after_flush.allocations;
    const std::size_t idle_allocations = after_idle_ticks.allocations - after_noops.allocations;

    bool ok = expect(set_allocations == 0, "set() allocates nothing");
    ok = expect(after_sets.bytes == before.bytes, "set() allocates no bytes") && ok;
    ok = expect(flush_allocations == 0, "the coalescing flush allocates nothing") && ok;
    ok = expect(noop_allocations == 0, "set() with an unchanged value allocates nothing") && ok;
    ok = expect(idle_allocations == 0, "an idle tick allocates nothing") && ok;

    // The measured work really happened, so the zero above is not "the loop was elided".
    ok = expect(observer_calls == 1, "10000 sets produced exactly one observer notification") && ok;
    ok = expect(stats.updated == 2,
                "the flush ran the observable ticket and then the ticket observer") &&
         ok;
    ok = expect(probe.updates == 1, "the ticket observer's update ran once") && ok;
    ok = expect(observable.get() == kSets && observable.version() == kSets,
                "the value and version reached the last write") &&
         ok;

    std::printf("appfw_observable set: %zu allocations in %d set() calls, %zu in the flush, "
                "%zu for 1000 unchanged sets, %zu for 1000 idle ticks (observer calls: %d)\n",
                set_allocations, kSets, flush_allocations, noop_allocations, idle_allocations,
                observer_calls);

    // Releasing is unmount work: it frees, and must not allocate.
    const HeapWindow before_release = heap_window();
    ticket_subscription.unsubscribe();
    callback_subscription.unsubscribe();
    const HeapWindow after_release = heap_window();
    ok = expect(after_release.allocations == before_release.allocations,
                "unsubscribing allocates nothing") &&
         ok;
    ok = expect(observable.observer_count() == 0, "both links are gone") && ok;
    return ok;
}

// Subscribing is the documented allocation point, and it happens at mount, so it is allowed to
// allocate: the test asserts that it does, which keeps "zero on set()" from being a vacuous claim
// about a path that never allocates anywhere.
bool subscribing_allocates_and_set_stays_free() {
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    const HeapWindow before = heap_window();
    auto subscription = observable.subscribe(&count_observer, nullptr);
    const HeapWindow after_subscribe = heap_window();
    bool ok = expect(after_subscribe.allocations > before.allocations,
                     "subscribing allocates its node (mount work, not the hot path)");
    ok = expect(observable.observer_count() == 1, "the link is registered") && ok;

    // With a fresh observer registered, the set path is still free.
    observer_calls = 0;
    const HeapWindow before_sets = heap_window();
    observable.set(1);
    observable.set(2);
    const HeapWindow after_sets = heap_window();
    ok = expect(after_sets.allocations == before_sets.allocations,
                "set() allocates nothing with a subscriber attached") &&
         ok;

    // A fresh window for the flush: `expect` above must not be inside it (a dynamic message would
    // allocate), and the pass itself is the thing being measured.
    const HeapWindow before_flush = heap_window();
    scheduler.flush();
    const HeapWindow after_flush = heap_window();
    ok = expect(after_flush.allocations == before_flush.allocations,
                "the notification pass allocates nothing (it compacts in place)") &&
         ok;
    ok = expect(observer_calls == 1, "the observer was notified once") && ok;
    (void)subscription;
    return ok;
}

// An unsubscribe from inside a notification nulls a slot and compacts after the pass; neither step
// may allocate, because a component unmounting in a callback is ordinary on the frame path.
bool unsubscribe_during_a_flush_allocates_nothing() {
    struct SelfRelease {
        appfw::Observable<int>::Subscription subscription;
        int calls = 0;
    };
    ui::UpdateScheduler scheduler;
    appfw::Observable<int> observable(scheduler, 0);
    SelfRelease remover;
    remover.subscription = observable.subscribe(
        [](void* context, const int&) {
            auto* self = static_cast<SelfRelease*>(context);
            ++self->calls;
            self->subscription.unsubscribe();
        },
        &remover);
    auto survivor = observable.subscribe(&count_observer, nullptr);

    observer_calls = 0;
    observable.set(1);
    const HeapWindow before = heap_window();
    scheduler.flush();
    const HeapWindow after = heap_window();
    bool ok = expect(after.allocations == before.allocations,
                     "an unsubscribe inside a notification allocates nothing");
    ok = expect(remover.calls == 1 && observer_calls == 1, "both observers ran once") && ok;
    ok = expect(observable.observer_count() == 1, "the released link was compacted away") && ok;
    (void)survivor;
    return ok;
}

} // namespace

// Entry point used by appfw_observable_tests.cpp's main().
bool observable_set_allocates_nothing() {
    bool ok = set_and_flush_allocate_nothing();
    ok = subscribing_allocates_and_set_stays_free() && ok;
    ok = unsubscribe_during_a_flush_allocates_nothing() && ok;
    return ok;
}

} // namespace appfw_test
