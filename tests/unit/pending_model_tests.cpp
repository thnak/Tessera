// SPDX-License-Identifier: MIT

// ADR-067 spike #661, the pending-UI model on its own: the Suspense boundary's fallback/restore/
// keep-on-refresh rule, the transition mark's `isPending`, and the stale-epoch drop. These are the
// behaviours the evaluator and generated C++ must agree on; the differential test
// (`pending_parity`) proves they do, and this test proves the model itself is right.
//
// Deterministic: no clock, no network, no random ids. The fake resource is driven by explicit
// epochs, which is exactly what the data store's per-key epoch counter is.

#include "tessera/ui/pending/fake_resource.hpp"
#include "tessera/ui/pending/suspense.hpp"
#include "tessera/ui/pending/transition.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

namespace pending = tessera::ui::pending;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

// The boundary's read slot 0 mirrors the resource, which is what a `useResource` subscription does.
void sync(pending::SuspenseBoundary& boundary, const pending::FakeResource& resource) {
    (void)boundary.set_read(0, resource.loading(), resource.has_data(), resource.epoch());
}

void test_fallback_while_loading_with_no_data() {
    pending::FakeResource resource;
    pending::SuspenseBoundary boundary;
    check(boundary.reset_reads(1) == 1, "one read slot is registered");
    // Before any fetch the resource is Idle with no data: nothing suspends, so the content shows.
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Content,
          "an idle resource with no data does not suspend");

    const std::uint64_t epoch = resource.begin_fetch();
    check(epoch == 1, "the first fetch is epoch 1");
    check(resource.suspends(), "a fetch with no data suspends");
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Fallback,
          "the boundary shows its fallback while the read suspends");
    check(boundary.fallback_live(), "fallback_live() agrees with the phase");
    check(boundary.suspends(), "the boundary reports the suspending read");
}

void test_subtree_restored_on_data() {
    pending::FakeResource resource;
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    const std::uint64_t epoch = resource.begin_fetch();
    sync(boundary, resource);
    check(boundary.fallback_live(), "the fallback is live while loading");

    check(resource.resolve(epoch, 7), "the completion for the current epoch lands");
    check(resource.has_data() && !resource.loading(), "the resource is settled with data");
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Content,
          "the subtree is restored once the data arrives");
    check(!boundary.fallback_live(), "the fallback is no longer live");
    check(resource.value() == 7, "the published value is the completion's");
}

void test_old_subtree_kept_on_refresh() {
    pending::FakeResource resource;
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    const std::uint64_t first = resource.begin_fetch();
    check(resource.resolve(first, 1), "the first fetch lands");
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Content, "the content is live with data");

    // A key change on a resource that already has data: Refreshing, not Loading.
    const std::uint64_t second = resource.begin_fetch();
    check(second == first + 1, "a key change bumps the epoch");
    check(resource.refreshing(), "a fetch with data is Refreshing");
    check(!resource.suspends(), "Refreshing does not suspend (ADR-067 item 3)");
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Content,
          "the boundary keeps the old subtree while refreshing");
    check(!boundary.fallback_live(), "the fallback does not replace a refreshing subtree");
    check(resource.value() == 1, "the previous data is still published while refreshing");

    check(resource.resolve(second, 2), "the refresh lands");
    sync(boundary, resource);
    check(resource.value() == 2, "the refreshed value replaces the old one");
    check(boundary.phase() == pending::SuspensePhase::Content, "the content stays live");
}

void test_stale_epoch_dropped() {
    pending::FakeResource resource;
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    const std::uint64_t first = resource.begin_fetch();
    const std::uint64_t second = resource.begin_fetch(); // a key change supersedes the first
    check(second == first + 1, "the second fetch has a newer epoch");

    check(!resource.resolve(first, 99), "a completion for the superseded epoch is dropped");
    check(!resource.has_data(), "a dropped completion publishes nothing");
    check(resource.loading(), "the newer fetch is still in flight");
    sync(boundary, resource);
    check(boundary.fallback_live(), "the boundary still shows the fallback");

    check(resource.resolve(second, 5), "the completion for the current epoch lands");
    sync(boundary, resource);
    check(resource.value() == 5, "the accepted completion is the newer one");
    check(boundary.phase() == pending::SuspensePhase::Content, "the subtree is restored");
}

void test_failed_refresh_keeps_data() {
    pending::FakeResource resource;
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    const std::uint64_t first = resource.begin_fetch();
    check(resource.resolve(first, 3), "the first fetch lands");
    const std::uint64_t second = resource.begin_fetch();
    check(resource.fail(second), "the refresh fails");
    check(resource.failed() && !resource.loading(), "the failure settles the fetch");
    check(resource.has_data() && resource.value() == 3,
          "a failed refresh keeps the previous data (async-data-v1 section 6)");
    sync(boundary, resource);
    check(boundary.phase() == pending::SuspensePhase::Content,
          "a failed refresh never blanks the screen");
}

void test_transition_pending() {
    pending::FakeResource resource;
    pending::TransitionMark mark;
    check(!mark.pending(), "a mark that never started is not pending");

    const std::uint64_t epoch = resource.begin_fetch();
    mark.begin(epoch);
    check(mark.pending(), "a started transition is pending");
    check(mark.started() && mark.epoch() == epoch, "the mark keeps the epoch it started under");

    check(mark.observe(true, epoch), "an observation for the current epoch is accepted");
    check(mark.pending(), "a Loading resource keeps isPending true");
    check(mark.observe(false, epoch), "the settled observation is accepted");
    check(!mark.pending(), "a settled resource clears isPending");

    // Refreshing is pending too (ADR-067 item 4): the change's fetch is still in flight.
    const std::uint64_t refresh = resource.begin_fetch();
    mark.begin(refresh);
    check(mark.observe(resource.loading(), refresh), "a refreshing observation is accepted");
    check(mark.pending(), "Refreshing keeps isPending true");

    mark.clear();
    check(!mark.pending() && !mark.started(), "clear() settles the mark");
}

void test_transition_stale_epoch() {
    pending::FakeResource resource;
    pending::TransitionMark mark;
    const std::uint64_t first = resource.begin_fetch();
    mark.begin(first);
    const std::uint64_t second = resource.begin_fetch(); // a newer change supersedes it
    check(!mark.accept(second), "the mark rejects a newer epoch");
    check(mark.accept(first), "the mark accepts its own epoch");
    check(!mark.observe(false, second), "a stale observation is dropped");
    check(mark.pending(), "a dropped observation does not clear isPending");
    check(mark.observe(false, first), "the mark's own epoch is still accepted");
    check(!mark.pending(), "the accepted observation settles the transition");
}

void test_read_slot_errors() {
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(1);
    check(!boundary.set_read(1, true, false, 1).has_value(),
          "an unregistered read slot is a recoverable error");
    check(boundary.set_read(0, true, false, 1).has_value(), "the registered slot accepts a read");
    check(boundary.read(0) != nullptr && boundary.read(0)->suspends(), "the read is stored");
    check(boundary.read(1) == nullptr, "an unregistered slot reads as null");
    check(boundary.reset_reads(99) == pending::SuspenseBoundary::kMaxReads,
          "the read count is clamped to kMaxReads");
}

void test_multiple_reads() {
    pending::SuspenseBoundary boundary;
    boundary.reset_reads(2);
    (void)boundary.set_read(0, false, true, 1); // settled
    (void)boundary.set_read(1, true, false, 2); // suspending
    check(boundary.suspends(), "one suspending read is enough to show the fallback");
    (void)boundary.set_read(1, false, true, 2);
    check(!boundary.suspends(), "every read settled restores the content");
}

} // namespace

int main() {
    test_fallback_while_loading_with_no_data();
    test_subtree_restored_on_data();
    test_old_subtree_kept_on_refresh();
    test_stale_epoch_dropped();
    test_failed_refresh_keeps_data();
    test_transition_pending();
    test_transition_stale_epoch();
    test_read_slot_errors();
    test_multiple_reads();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "pending_model: ok\n";
    return 0;
}
