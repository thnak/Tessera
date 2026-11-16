// SPDX-License-Identifier: MIT
//
// Acceptance word 4: **restore** (docs/specs/navigation-v1.md section 8; docs/specs/
// state-restoration-v1.md sections 2, 3, 4, 6 and 7.1; issue #196).
//
// The stack survives a restart: a snapshot round-trips through canonical JSON, persists through
// AF7's `Storage` (atomic, on the I/O executor), and is refused - safely - when the version, the
// app version or the route table changed.

#include "appfw_navigation_test_support.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nav_test {
namespace {

NavSnapshot snapshot_of(const Harness& harness, std::string app_version = "1.4.0") {
    NavSnapshot snapshot;
    snapshot.app_version = std::move(app_version);
    snapshot.route_fingerprint = harness.table.fingerprint();
    snapshot.stack.id = std::string(tessera::appfw::kNavMainStackId);
    for (const NavEntry& entry : harness.controller.stack()) {
        NavEntrySnapshot encoded;
        encoded.entry_id = entry.entry_id;
        encoded.route_id = entry.route_id;
        encoded.params = entry.params;
        encoded.state = entry.state;
        encoded.presentation = entry.presentation;
        snapshot.stack.entries.push_back(std::move(encoded));
    }
    snapshot.active_index = harness.controller.depth() == 0 ? 0 : harness.controller.depth() - 1;
    return snapshot;
}

void the_stack_round_trips_byte_for_byte() {
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    harness.controller.set_entry_state(harness.controller.active()->entry_id,
                                       StateMap{{"tab", "items"}});

    const NavSnapshot snapshot = snapshot_of(harness);
    auto encoded = tessera::appfw::encode_nav_snapshot(snapshot);
    expect(encoded.has_value(), "the snapshot encodes");

    auto decoded = tessera::appfw::decode_nav_snapshot(*encoded, harness.table);
    expect(decoded.has_value(), "and decodes");
    expect(decoded->dropped == 0, "with no dropped entries");
    expect(decoded->snapshot.stack.entries.size() == 2, "both entries survived");
    expect(decoded->snapshot.stack.entries[0].route_id == "orders", "in order");
    expect(decoded->snapshot.stack.entries[1].route_id == "orders.detail", "all the way up");
    expect(decoded->snapshot.stack.entries[1].params.size() == 1, "with the params");
    expect(decoded->snapshot.stack.entries[1].state.size() == 1, "and the per-entry state");
    expect(decoded->snapshot.active_index == 1, "with the active index");

    // Determinism is the property the round trip is an equality on: re-encoding the decoded shape
    // produces the same bytes, because the codec is canonical and no timestamp or float is
    // involved.
    auto re_encoded = tessera::appfw::encode_nav_snapshot(decoded->snapshot);
    expect(re_encoded.has_value(), "the decoded snapshot re-encodes");
    expect(*re_encoded == *encoded, "byte for byte, so the format is stable across a restart");

    // And the decoded stack restores into a fresh controller, which is what "survives a restart"
    // means.
    Harness restarted;
    const std::vector<NavEntry> entries = tessera::appfw::restore_entries(*decoded);
    expect(restarted.controller.replace_all(entries, entries.size() - 1).has_value(),
           "the restored entries install into a new controller");
    expect(restarted.controller.depth() == 2, "with the same depth");
    expect(restarted.controller.active()->route_id == "orders.detail", "and the same active route");
    expect(restarted.controller.active()->entry_id == harness.controller.active()->entry_id,
           "the entry id is stable across the restart (section 10.1)");
    expect(restarted.controller.active()->state.size() == 1, "and the per-entry state survived");
}

void a_version_mismatch_falls_back_rather_than_guessing() {
    Harness harness;
    (void)harness.controller.push("/orders");
    auto encoded = tessera::appfw::encode_nav_snapshot(snapshot_of(harness));
    expect(encoded.has_value(), "the snapshot encodes");

    // state-restoration-v1.md section 3: "`restoreVersion` migrates forward; unknown fields are
    // ignored safely." There is one version so far, so another is refused rather than interpreted.
    std::string other_version = *encoded;
    const std::string from = "\"restoreVersion\":1";
    const std::size_t at = other_version.find(from);
    expect(at != std::string::npos, "the version member is in the encoded form");
    other_version.replace(at, from.size(), "\"restoreVersion\":99");
    const auto versioned = tessera::appfw::decode_nav_snapshot(other_version, harness.table);
    expect(!versioned.has_value(), "an unknown restore version is refused");
    expect(versioned.error().code == RestoreErrorCode::UnsupportedVersion,
           "with UnsupportedVersion");

    // navigation-v1.md section 5: "Startup restore uses the saved stack only if the route table/app
    // version still matches." A different table is refused, because the same id may now mean a
    // different screen. That is what makes the fallback the route root rather than a wrong screen.
    RouteTableConfig renamed = spec_config();
    renamed.routes.back().path = "/missing"; // same id, different path: the fingerprint moves
    const RouteTable other_table = make_table(std::move(renamed));
    expect(other_table.fingerprint() != harness.table.fingerprint(),
           "a re-pathed table has a different fingerprint");
    const auto mismatched = tessera::appfw::decode_nav_snapshot(*encoded, other_table);
    expect(!mismatched.has_value(), "a different route table is refused");
    expect(mismatched.error().code == RestoreErrorCode::RouteTableMismatch,
           "with RouteTableMismatch");

    // A strict app-version policy turns a version change into a refusal; the default accepts it,
    // because a patch release must not lose the user's stack.
    RestorePolicy strict;
    strict.expected_app_version = "1.4.0";
    auto stale = *encoded;
    const std::string version_member = "\"appVersion\":\"1.4.0\"";
    const std::size_t version_at = stale.find(version_member);
    expect(version_at != std::string::npos, "the appVersion member is in the encoded form");
    stale.replace(version_at, version_member.size(), "\"appVersion\":\"0.9.0\"");
    expect(tessera::appfw::decode_nav_snapshot(stale, harness.table, strict).error().code ==
               RestoreErrorCode::AppVersionMismatch,
           "a strict policy refuses a different app version");
    expect(tessera::appfw::decode_nav_snapshot(stale, harness.table).has_value(),
           "and the default policy accepts it");
}

void a_deep_link_into_a_flow_that_no_longer_exists_is_dropped() {
    // The case the issue names: "say what the spec says about not restoring (e.g. a deep link into
    // a flow that no longer exists)". state-restoration-v1.md section 7.1: "App/route-table version
    // mismatch: drop incompatible entries, keep a safe root"; navigation-v1.md section 10.1:
    // "invalid entries are dropped or replaced by the route root".
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    auto encoded = tessera::appfw::encode_nav_snapshot(snapshot_of(harness));
    expect(encoded.has_value(), "the snapshot encodes");

    // The same table, but one entry names a route it no longer has: the entry is dropped and
    // counted, and the rest of the stack survives - a partial restore, not a lost session.
    std::string with_bogus = *encoded;
    const std::string needle = "\"route\":\"orders.detail\"";
    const std::size_t at = with_bogus.find(needle);
    expect(at != std::string::npos, "the route member is in the encoded form");
    with_bogus.replace(at, needle.size(), "\"route\":\"retired.flow\"");
    auto decoded = tessera::appfw::decode_nav_snapshot(with_bogus, harness.table);
    expect(decoded.has_value(), "the snapshot still decodes");
    expect(decoded->snapshot.stack.entries.size() == 1, "the retired route's entry is gone");
    expect(decoded->snapshot.stack.entries[0].route_id == "orders",
           "and the rest of the stack survived");
    // The pruned entry is what section 10.1's "invalid entries are dropped" means, and `dropped` is
    // the count a diagnostic reports: one entry named a flow that no longer exists.
    expect(decoded->dropped == 1, "the dropped entry is counted");
    expect(decoded->snapshot.active_index == 0,
           "and the active index is clamped into the pruned stack, not past its end");

    // The surviving entries install, so the user keeps a usable stack rather than an empty app
    // (section 4: "A failed step falls back to the route root, not an empty/white app").
    const std::vector<NavEntry> entries = tessera::appfw::restore_entries(*decoded);
    Harness restarted;
    expect(restarted.controller.replace_all(entries, entries.size() - 1).has_value(),
           "the surviving entries still restore");
    expect(restarted.controller.active()->route_id == "orders", "with the last valid route active");

    // A stack whose entries the table cannot resolve at all is refused by the same code a
    // navigation uses, rather than installing a screen name the table cannot mount.
    NavEntry retired;
    retired.route_id = "retired.flow";
    Harness rejected;
    expect(rejected.controller.replace_all({retired}, 0).error().code ==
               NavigationErrorCode::UnknownRouteId,
           "an entry naming a route the table lost is refused");
    expect(rejected.controller.depth() == 0, "and nothing was installed");
}

void a_corrupt_snapshot_is_a_named_failure() {
    const RouteTable table = make_table();
    struct Case {
        std::string text;
        RestoreErrorCode code;
    };
    const Case cases[] = {
        {"not json at all", RestoreErrorCode::MalformedJson},
        {"", RestoreErrorCode::MalformedJson},
        {"[]", RestoreErrorCode::MalformedSnapshot},
        {"{}", RestoreErrorCode::MalformedSnapshot},
        {"{\"restoreVersion\":\"one\"}", RestoreErrorCode::MalformedSnapshot},
        {"{\"restoreVersion\":1}", RestoreErrorCode::MalformedSnapshot}, // no stacks
        {"{\"restoreVersion\":1,\"routeTable\":\"x\",\"stacks\":{}}",
         RestoreErrorCode::MalformedSnapshot},
    };
    for (const Case& test : cases) {
        const auto decoded = tessera::appfw::decode_nav_snapshot(test.text, table);
        expect(!decoded.has_value(), "a corrupt snapshot does not decode");
        expect(!decoded.has_value() && decoded.error().code == test.code,
               "the failure names the corruption class");
    }

    // The size cap is checked before the decoder sees the bytes (G-14/Q-13's 256 KiB).
    const std::string oversized(tessera::appfw::kNavSnapshotMaxBytes + 1, 'x');
    const auto too_large = tessera::appfw::decode_nav_snapshot(oversized, table);
    expect(!too_large.has_value(), "an oversized snapshot is refused");
    expect(too_large.error().code == RestoreErrorCode::TooLarge, "with TooLarge");
}

void the_snapshot_persists_through_appfw_storage() {
    // state-restoration-v1.md section 7.1: "`services/storage` owns atomic file/path/encryption
    // primitives". So the write is one atomic replace on the I/O executor, and the read is one load
    // - this module never opens a path or blocks the UI thread.
    StorageHarness storage;
    NavRestoreStore restore(storage.store, "navigation.snapshot");

    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");

    int completions = 0;
    std::expected<void, RestoreError> saved;
    const bool submitted =
        restore.save(snapshot_of(harness), {}, [&](std::expected<void, RestoreError> result) {
            ++completions;
            saved = std::move(result);
        });
    expect(submitted, "the save is submitted");
    expect(storage.executor.pending_work() == 1,
           "the body is queued, not run inline (never block the UI)");
    expect(completions == 0, "and the completion has not run yet");
    storage.run_one();
    expect(completions == 1, "the completion ran exactly once");
    expect(saved.has_value(), "the save completes successfully");

    // The bytes on disk are the encoded snapshot: the atomic sequence wrote a temp file, flushed it
    // and replaced the target, so a reader sees one complete document.
    expect(storage.files.files.count("/nav/navigation.snapshot") == 1, "the entry exists");
    expect(!storage.files.content_of("/nav/navigation.snapshot").empty(), "and holds the snapshot");
    const auto steps = storage.files.steps;
    expect(steps.size() >= 4, "the atomic sequence ran its steps");
    expect(steps[0] == "create-directory", "step 1 creates the parent");
    expect(steps[1] == "write", "step 2 writes the temp file");
    expect(steps[2] == "flush", "step 3 flushes the temp file");
    expect(steps[3] == "replace", "step 4 replaces atomically");

    // And the read comes back through the same seam, decoded and validated.
    std::expected<DecodedSnapshot, RestoreError> loaded;
    expect(restore.load(harness.table, {},
                        [&](std::expected<DecodedSnapshot, RestoreError> result) {
                            loaded = std::move(result);
                        }),
           "the load is submitted");
    storage.run_one();
    expect(loaded.has_value(), "the load decodes");
    expect(loaded->snapshot.stack.entries.size() == 2, "with the whole stack");
    expect(loaded->snapshot.stack.entries[1].route_id == "orders.detail", "and its active route");
}

void an_absent_snapshot_is_a_first_run_not_a_corruption() {
    StorageHarness storage;
    const RouteTable table = make_table();
    NavRestoreStore restore(storage.store, "navigation.snapshot");
    std::expected<DecodedSnapshot, RestoreError> loaded;
    expect(restore.load(table, {},
                        [&](std::expected<DecodedSnapshot, RestoreError> result) {
                            loaded = std::move(result);
                        }),
           "the load is submitted");
    storage.run_one();
    expect(!loaded.has_value(), "there is no snapshot yet");
    // The distinction is carried in `member`: a first run falls back to the route root silently
    // (section 4), a corrupt blob gets a diagnostic.
    expect(loaded.error().member == "not-found", "the failure is named not-found");

    // A corrupt entry is a different case: the bytes are present and wrong.
    storage.files.files["/nav/navigation.snapshot"] = "garbage";
    expect(restore.load(table, {},
                        [&](std::expected<DecodedSnapshot, RestoreError> result) {
                            loaded = std::move(result);
                        }),
           "the second load is submitted");
    storage.run_one();
    expect(!loaded.has_value(), "a corrupt snapshot does not load");
    expect(loaded.error().member != "not-found", "and it is not reported as a first run");
}

void a_failed_write_leaves_the_previous_snapshot() {
    // state-restoration-v1.md section 7.1: "Process death during write -> previous snapshot remains
    // valid."
    StorageHarness storage;
    NavRestoreStore restore(storage.store, "navigation.snapshot");
    Harness harness;
    (void)harness.controller.push("/orders");
    expect(restore.save(snapshot_of(harness), {}, {}), "the first save is submitted");
    storage.run_one();
    const std::string first = storage.files.content_of("/nav/navigation.snapshot");
    expect(!first.empty(), "the first snapshot is on disk");

    // A failing replace leaves the target's old content, because the atomic sequence never
    // truncates it.
    (void)harness.controller.push("/settings");
    storage.files.fail = MemoryFileSystem::Fail::Replace;
    std::expected<void, RestoreError> failed;
    expect(
        restore.save(snapshot_of(harness), {},
                     [&](std::expected<void, RestoreError> result) { failed = std::move(result); }),
        "the second save is submitted");
    storage.run_one();
    expect(!failed.has_value(), "the failing write is reported");
    expect(storage.files.content_of("/nav/navigation.snapshot") == first,
           "and the previous snapshot is intact");

    // With the failure cleared, the next save replaces it.
    storage.files.fail = MemoryFileSystem::Fail::None;
    expect(restore.save(snapshot_of(harness), {}, {}), "the third save is submitted");
    storage.run_one();
    expect(storage.files.content_of("/nav/navigation.snapshot") != first,
           "the snapshot was replaced");
}

void a_snapshot_over_the_cap_is_refused_before_it_is_written() {
    // G-14/Q-13: "<=256 KiB/window", enforced on the encoded form so the number the caller is
    // bounded by is the number that lands on disk.
    NavSnapshot snapshot;
    snapshot.app_version = "1.4.0";
    NavEntrySnapshot entry;
    entry.entry_id = 1;
    entry.route_id = "home";
    entry.state = {{"blob", std::string(tessera::appfw::kNavSnapshotMaxBytes + 1024, 'x')}};
    snapshot.stack.entries.push_back(entry);
    const auto encoded = tessera::appfw::encode_nav_snapshot(snapshot);
    expect(!encoded.has_value(), "an oversized snapshot does not encode");
    expect(encoded.error().code == RestoreErrorCode::TooLarge, "with TooLarge");
}

void a_restored_stack_keeps_its_entry_ids_and_allocates_above_them() {
    Harness harness;
    (void)harness.controller.push("/orders");
    (void)harness.controller.push("/orders/42");
    const std::uint64_t highest = harness.controller.active()->entry_id;

    // Restore into a fresh controller: ids are "stable for restore/automation" (section 10.1), so
    // they are kept, and the allocator resumes above them rather than reissuing one an older entry
    // still holds.
    auto encoded = tessera::appfw::encode_nav_snapshot(snapshot_of(harness));
    auto decoded = tessera::appfw::decode_nav_snapshot(*encoded, harness.table);
    expect(decoded.has_value(), "the snapshot decodes");
    Harness restarted;
    const std::vector<NavEntry> entries = tessera::appfw::restore_entries(*decoded);
    expect(restarted.controller.replace_all(entries, entries.size() - 1).has_value(),
           "the restore installs");
    expect(*restarted.controller.push("/settings") == PushOutcome::Committed,
           "a later push succeeds");
    expect(restarted.controller.active()->entry_id > highest,
           "and its id is above the highest restored one");
}

} // namespace

int run_restore_tests() {
    the_stack_round_trips_byte_for_byte();
    a_version_mismatch_falls_back_rather_than_guessing();
    a_deep_link_into_a_flow_that_no_longer_exists_is_dropped();
    a_corrupt_snapshot_is_a_named_failure();
    the_snapshot_persists_through_appfw_storage();
    an_absent_snapshot_is_a_first_run_not_a_corruption();
    a_failed_write_leaves_the_previous_snapshot();
    a_snapshot_over_the_cap_is_refused_before_it_is_written();
    a_restored_stack_keeps_its_entry_ids_and_allocates_above_them();
    return g_failures;
}

} // namespace nav_test
