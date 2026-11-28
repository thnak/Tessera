// SPDX-License-Identifier: MIT

// ADR-067 / #656: the `ResourceView` seam. A Suspense boundary reads a resource only through the
// view, so the deterministic FakeResource and the data store behind `StoreResource` must produce
// the same boundary behaviour: fallback, restore, a refresh that keeps the subtree, and a stale
// epoch that is dropped.

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/data_store.hpp"
#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/resource_provider.hpp"
#include "tessera/ui/pending/resource_view.hpp"
#include "tessera/ui/pending/store_resource.hpp"
#include "tessera/ui/pending/suspense.hpp"
#include "tessera/ui/pending/transition.hpp"

#include <iostream>
#include <memory>
#include <string>
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

// Typed `.data` and `.error` through the store (pending-ui-v1.md sections 3.1 and 6.1).
void test_typed_store_resource() {
    runtime::ManualClock clock;
    runtime::CompletionQueue queue(16);
    runtime::DataStore store(queue, clock, 8);
    const runtime::DataKey text_key{"row", "app", {}, {}, "id=i1;"};
    const runtime::DataKey count_key{"count", "app", {}, {}, "=i1;"};
    const pending::StoreResource text = pending::StoreResource::typed<std::string>(store, text_key);
    const pending::StoreResource count = pending::StoreResource::typed<int>(store, count_key);
    const pending::StoreResource flag = pending::StoreResource::typed<bool>(store, count_key);

    check(text.state().text.empty() && count.state().number == 0.0,
          "an unwritten typed resource reads as the zero value");
    runtime::FetchHandle fetch = store.begin_fetch(text_key);
    check(fetch.resolve(std::make_shared<const std::string>("hello")), "the string lands");
    check(text.state().text == "hello" && text.state().has_data,
          "a string value is published as text");
    store.set(count_key, std::make_shared<const int>(42));
    check(count.state().number == 42.0, "an int value is published as a number");
    check(flag.state().number == 0.0 && flag.state().has_data,
          "a value of another type reads as absent, never as a wrong scalar");

    runtime::FetchHandle again = store.begin_fetch(text_key);
    check(text.state().text == "hello" && text.state().refreshing(),
          "a refresh keeps the published text");
    check(again.fail(runtime::DataError{"E_NET", "offline"}), "the failure lands");
    check(text.state().error == "offline" && text.state().failed && text.state().text == "hello",
          ".error is the store's failure message and the data stays");
    runtime::FetchHandle healed = store.begin_fetch(text_key);
    check(healed.resolve(std::make_shared<const std::string>("again")), "the retry lands");
    check(text.state().error.empty() && text.state().text == "again",
          "a successful fetch clears the error");
}

void test_state_signature() {
    pending::FakeResource resource;
    const pending::ResourceView view = pending::ResourceView::of(resource);
    const std::uint64_t idle = view.read().signature();
    check(view.read().signature() == idle, "an unchanged state has a stable signature");
    const std::uint64_t epoch = resource.begin_fetch();
    const std::uint64_t loading = view.read().signature();
    check(loading != idle, "a fetch changes the signature");
    check(resource.resolve_number(epoch, 1.5), "the number lands");
    const std::uint64_t done = view.read().signature();
    check(done != loading, "a completion changes the signature");
    const std::uint64_t next = resource.begin_fetch();
    check(resource.fail(next, "boom"), "the failure lands");
    check(view.read().error == "boom" && view.read().signature() != done,
          "a failure with text changes the signature");
}

void test_resource_key() {
    pending::ResourceKey record;
    record.field("id", 1).field("name", "ab").field("on", true).field("w", 2.5);
    check(record.view() == "id=i1;name=s2:ab;on=b1;w=d2.5;", "a record key is canonical");
    pending::ResourceKey scalar;
    scalar.value(std::string_view("x"));
    check(scalar.view() == "=s1:x;", "a bare scalar is the empty-named field");

    pending::ResourceKey same;
    same.field("id", 1).field("name", "ab").field("on", true).field("w", 2.5);
    check(record.same_as(same), "equal keys compare equal");
    pending::ResourceKey other;
    other.field("id", 2).field("name", "ab").field("on", true).field("w", 2.5);
    check(!record.same_as(other), "a changed field compares unequal");
    // A string cannot forge a field boundary: lengths are part of the encoding.
    pending::ResourceKey forged_a;
    forged_a.field("a", "x;b=i1");
    pending::ResourceKey forged_b;
    forged_b.field("a", "x").field("b", 1);
    check(!forged_a.same_as(forged_b), "string content cannot masquerade as another field");

    pending::ResourceKey long_key;
    long_key.field("text", std::string(pending::ResourceKey::kCapacity, 'x'));
    check(long_key.overflow() && !long_key.same_as(long_key),
          "an oversized key overflows and never matches (the resource stays unbound)");
}

} // namespace

int main() {
    test_typed_store_resource();
    test_state_signature();
    test_resource_key();
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
