// SPDX-License-Identifier: MIT
//
// Issue #411 item 1: versioned stored data. The chain is pure, so its outcomes are asserted without
// a disk; the store-level cases then prove the I/O contract around it: an old entry is upgraded and
// rewritten once, a newer or un-migratable entry is reported by name and its file is not touched.

#include "appfw_storage_test_support.hpp"
#include "tessera/appfw/storage_migration.hpp"

#include <memory>
#include <optional>
#include <string>

namespace storage_test {
namespace {

using tessera::appfw::MigrationChain;
using tessera::appfw::MigrationOutcome;
using tessera::appfw::StoredValue;

// Each step appends its own marker, so the result names the steps that ran and their order.
std::shared_ptr<MigrationChain> marker_chain(std::uint32_t first, std::uint32_t last) {
    auto chain = std::make_shared<MigrationChain>();
    for (std::uint32_t version = first; version <= last; ++version) {
        chain->add(version, [version](std::string_view in) -> std::optional<std::string> {
            return std::string(in) + "|" + std::to_string(version);
        });
    }
    return chain;
}

void the_chain_names_every_outcome() {
    auto chain = marker_chain(1, 2);

    auto current = chain->run(3, "p", 3);
    expect(current.outcome == MigrationOutcome::Current && current.data == "p" &&
               current.steps_run == 0,
           "a payload at the target version is Current and runs nothing");

    auto migrated = chain->run(1, "p", 3);
    expect(migrated.outcome == MigrationOutcome::Migrated && migrated.data == "p|1|2" &&
               migrated.steps_run == 2,
           "steps run in order, one version at a time");
    expect(chain->run(2, "p", 3).data == "p|2", "a mid-chain start runs only the remaining steps");

    auto future = chain->run(4, "p", 3);
    expect(future.outcome == MigrationOutcome::FutureVersion && !future.usable() &&
               future.data.empty(),
           "a newer version is named and yields no data");

    auto gap = marker_chain(2, 2)->run(1, "p", 3);
    expect(gap.outcome == MigrationOutcome::MissingStep && gap.failed_at == 1,
           "a gap is reported at the version missing its step, before any step runs");

    auto failing = std::make_shared<MigrationChain>();
    int ran = 0;
    failing->add(1, [&](std::string_view in) -> std::optional<std::string> {
        ++ran;
        return std::string(in);
    });
    failing->add(2, [](std::string_view) -> std::optional<std::string> { return std::nullopt; });
    auto failed = failing->run(1, "p", 3);
    expect(failed.outcome == MigrationOutcome::StepFailed && failed.failed_at == 2 && ran == 1 &&
               failed.data.empty(),
           "a rejecting step fails the run at its version and yields no data");

    expect(chain->run(0, "p", 3).outcome == MigrationOutcome::InvalidVersion, "version 0 invalid");
    expect(!chain->add(1, [](std::string_view) { return std::optional<std::string>(); }),
           "a second step for one hop is rejected");
    expect(!chain->add(0, [](std::string_view in) { return std::optional<std::string>(in); }),
           "version 0 has no step");
    expect(!chain->add(7, {}), "an empty step is rejected");
}

// Two stores over one fake disk: an old build (version 1) writes, the new build (version 3) reads.
struct Disk {
    explicit Disk(std::uint32_t new_version)
        : old_store(StorageConfig{"/store", 4096, Durability::BestEffort, 1, 8}, executor, files),
          new_store(StorageConfig{"/store", 4096, Durability::BestEffort, new_version, 8}, executor,
                    files) {}

    void run() {
        executor.run(64);
        executor.run_completions(64);
    }

    ManualIoExecutor executor;
    FakeFileSystem files;
    Storage old_store;
    Storage new_store;
};

std::expected<StoredValue, StorageError> migrate_load(Disk& disk,
                                                      std::shared_ptr<const MigrationChain> chain) {
    std::expected<StoredValue, StorageError> out = std::unexpected(StorageError{});
    disk.new_store.load_migrated("doc", std::move(chain), {}, [&](auto v) { out = std::move(v); });
    disk.run();
    return out;
}

void an_old_entry_is_upgraded_and_rewritten_once() {
    Disk disk(3);
    disk.old_store.save("doc", "p", {}, {});
    disk.run();

    auto loaded = migrate_load(disk, marker_chain(1, 2));
    expect(loaded.has_value() && loaded->data == "p|1|2" && loaded->version == 3 &&
               loaded->migrated,
           "the old entry loads at the current version, flagged migrated");
    expect(disk.new_store.diagnostics().migrations == 1, "the migration is counted");

    // The rewrite means a plain load at the new version now succeeds, and the chain is not rerun.
    std::expected<StoredValue, StorageError> plain = std::unexpected(StorageError{});
    disk.new_store.load("doc", {}, [&](auto v) { plain = std::move(v); });
    disk.run();
    expect(plain.has_value() && plain->data == "p|1|2" && plain->version == 3,
           "the entry was rewritten atomically at the current version");
    auto again = migrate_load(disk, marker_chain(1, 2));
    expect(again.has_value() && !again->migrated && again->data == "p|1|2",
           "a second migrating load finds the entry Current");
    expect(disk.new_store.diagnostics().migrations == 1, "the chain ran once per entry");
    expect(disk.files.temp_file_count() == 0, "the rewrite left no temp file");
}

void a_newer_entry_is_named_and_left_alone() {
    Disk disk(1);
    // Written by a build with format version 5; this build is version 1.
    Storage future(StorageConfig{"/store", 4096, Durability::BestEffort, 5, 8}, disk.executor,
                   disk.files);
    future.save("doc", "from the future", {}, {});
    disk.run();
    const std::string before = disk.files.content_of("/store/doc");

    auto loaded = migrate_load(disk, marker_chain(1, 1));
    expect(!loaded.has_value() && loaded.error().code == StorageErrorCode::FutureVersion,
           "a newer entry reports FutureVersion, not Corrupted");
    expect(disk.files.content_of("/store/doc") == before, "the newer entry is not touched");
    const auto diagnostics = disk.new_store.diagnostics();
    expect(diagnostics.entries.size() == 1 &&
               diagnostics.entries[0].code == StorageErrorCode::FutureVersion,
           "a diagnostic records it");
    expect(diagnostics.migrations == 0, "nothing was migrated");
}

void a_gap_or_a_rejecting_step_leaves_the_file_alone() {
    Disk disk(3);
    disk.old_store.save("doc", "p", {}, {});
    disk.run();
    const std::string before = disk.files.content_of("/store/doc");

    auto gap = migrate_load(disk, marker_chain(2, 2));
    expect(!gap.has_value() && gap.error().code == StorageErrorCode::MigrationFailed &&
               gap.error().native == 1,
           "a gap reports MigrationFailed at the missing version");

    auto rejecting = std::make_shared<MigrationChain>();
    rejecting->add(1, [](std::string_view) -> std::optional<std::string> { return std::nullopt; });
    rejecting->add(2, [](std::string_view in) { return std::optional<std::string>(in); });
    auto rejected = migrate_load(disk, rejecting);
    expect(!rejected.has_value() && rejected.error().code == StorageErrorCode::MigrationFailed,
           "a rejecting step reports MigrationFailed");
    expect(disk.files.content_of("/store/doc") == before, "the original entry is untouched");
}

void a_failed_rewrite_still_returns_the_migrated_data() {
    Disk disk(2);
    disk.old_store.save("doc", "p", {}, {});
    disk.run();
    const std::string before = disk.files.content_of("/store/doc");
    disk.files.fail = FailStep::Replace;

    auto loaded = migrate_load(disk, marker_chain(1, 1));
    expect(loaded.has_value() && loaded->data == "p|1" && loaded->migrated,
           "the caller still gets usable data when the rewrite fails");
    expect(disk.files.content_of("/store/doc") == before,
           "the old entry survives the failed rewrite");
    expect(disk.files.temp_file_count() == 0, "no temp file is left behind");
}

void a_corrupt_entry_and_a_null_chain_are_handled() {
    Disk disk(2);
    disk.files.files["/store/doc"] = "not an envelope";
    auto loaded = migrate_load(disk, marker_chain(1, 1));
    expect(!loaded.has_value() && loaded.error().code == StorageErrorCode::Corrupted,
           "a corrupt entry is still Corrupted");
    expect(!disk.new_store.load_migrated("doc", nullptr, {}, {}), "a null chain is refused");
}

} // namespace

int run_migration_tests() {
    the_chain_names_every_outcome();
    an_old_entry_is_upgraded_and_rewritten_once();
    a_newer_entry_is_named_and_left_alone();
    a_gap_or_a_rejecting_step_leaves_the_file_alone();
    a_failed_rewrite_still_returns_the_migrated_data();
    a_corrupt_entry_and_a_null_chain_are_handled();
    return g_failures;
}

} // namespace storage_test
