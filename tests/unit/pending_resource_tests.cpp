// SPDX-License-Identifier: MIT

// ADR-067 / #656: the `ResourceView` seam. A Suspense boundary reads a resource only through the
// view, so the deterministic FakeResource and the data store behind `StoreResource` must produce
// the same boundary behaviour: fallback, restore, a refresh that keeps the subtree, and a stale
// epoch that is dropped.

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/pending/store_resource.hpp"
#include "tessera/ui/pending/suspense.hpp"
#include "tessera/ui/pending/transition.hpp"

#include <iostream>
#include <memory>
#include <string_view>

namespace {

namespace pending = tessera::ui::pending;
namespace runtime = tessera::runtime;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void sync(pending::SuspenseBoundary& boundary, const pending::ResourceView& view) {
    const pending::ResourceState state = view.read();
    (void)boundary.set_read(0, state.loading, state.has_data, state.epoch);
}

void test_unbound_view_is_idle() {
    const pending::ResourceView view;
    check(!view.bound(), "a default view is unbound");
    check(!view.read().suspends() && !view.read().refreshing(),
          "an unbound view reads as an idle resource and never suspends");
}

void test_fake_resource_view() {
    pending::FakeResource resource;
    const pending::ResourceView view = pending::ResourceView::of(resource);
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    sync(boundary, view);
    check(!boundary.fallback_live(), "idle: content");
    const std::uint64_t epoch = resource.begin_fetch();
    sync(boundary, view);
    check(boundary.fallback_live(), "fetch with no data: fallback");
    check(resource.resolve(epoch, 3), "completion lands");
    sync(boundary, view);
    check(!boundary.fallback_live(), "data restores the subtree");
    (void)resource.begin_fetch();
    sync(boundary, view);
    check(!boundary.fallback_live() && view.read().refreshing(), "a refresh keeps the subtree");
}

void test_store_resource_view() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 8);
    const runtime::DataKey key{"feed", "app", {}, {}, "q=a"};
    const pending::StoreResource resource(store, key);
    const pending::ResourceView view = pending::ResourceView::of(resource);

    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    pending::TransitionMark mark;

    sync(boundary, view);
    check(!boundary.fallback_live(), "an unwritten key does not suspend");

    runtime::FetchHandle first = store.begin_fetch(key);
    sync(boundary, view);
    check(boundary.fallback_live(), "a fetch with no data shows the fallback");
    check(view.read().epoch == 1, "the view carries the store epoch");

    check(first.resolve(std::make_shared<const int>(7)), "the current completion lands");
    sync(boundary, view);
    check(!boundary.fallback_live(), "the published data restores the subtree");

    // A transition starts under the epoch the change produces; a refresh keeps the subtree.
    runtime::FetchHandle second = store.begin_fetch(key);
    mark.begin(view.read().epoch);
    sync(boundary, view);
    check(!boundary.fallback_live(), "a refresh with data does not show the fallback");
    check(mark.observe(view.read().loading, view.read().epoch) && mark.pending(),
          "the transition is pending while the refresh runs");

    // A completion for a superseded epoch is dropped by the store and cannot clear the mark.
    check(second.resolve(std::make_shared<const int>(8)), "the refresh lands");
    sync(boundary, view);
    check(!view.read().loading, "the refresh settled");
    check(mark.observe(view.read().loading, view.read().epoch) && !mark.pending(),
          "isPending clears when the read settles");

    runtime::FetchHandle third = store.begin_fetch(key);
    const std::uint64_t superseded = view.read().epoch;
    store.set(key, std::make_shared<const int>(9)); // a push supersedes the in-flight fetch
    check(!third.resolve(std::make_shared<const int>(1)), "the store drops the stale completion");
    check(view.read().epoch != superseded, "the push bumped the epoch");
    check(!mark.accept(superseded + 5), "the mark ignores an epoch it never began");

    // An error keeps data: a failed refresh does not blank the screen.
    runtime::FetchHandle fourth = store.begin_fetch(key);
    check(fourth.fail(runtime::DataError{"E_NET", "offline"}), "the failure lands");
    sync(boundary, view);
    check(view.read().failed && view.read().has_data && !boundary.fallback_live(),
          "a failed refresh keeps the data and the subtree");

    // Rebinding to a new key starts from an idle read, then follows that key.
    pending::StoreResource movable(store, key);
    const pending::ResourceView movable_view = pending::ResourceView::of(movable);
    movable.rebind(runtime::DataKey{"feed", "app", {}, {}, "q=b"});
    check(!movable_view.read().has_data, "a new key has no data until it is written");
}

} // namespace

int main() {
    test_unbound_view_is_idle();
    test_fake_resource_view();
    test_store_resource_view();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending resource: ok\n";
    return 0;
}
