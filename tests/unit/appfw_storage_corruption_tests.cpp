// SPDX-License-Identifier: MIT
//
// AF7 (issue #179): corruption fallback. The acceptance criterion is "a corrupted file falls back
// with a diagnostic", so each case here corrupts exactly one thing, reads it through the store and
// asserts three things: the read reports `Corrupted` (so the caller uses its default), the
// diagnostic names the field that disagreed, and the file itself is left alone (a corrupt read
// never deletes the user's data behind their back).

#include "appfw_storage_test_support.hpp"

#include <string>

namespace storage_test {
namespace {

// A store with a fake file system, already holding the encoded form of `payload` under `name`.
// The healthy path first, so the corruption cases below are known to be the only difference.
void a_healthy_entry_round_trips() {
    Harness harness;
    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.save("doc", "hello", {}, {});
    harness.run_one();
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(loaded.has_value(), "a saved entry loads back");
    expect(loaded->data == "hello", "the payload round trips");
    expect(loaded->version == 1, "the envelope version is reported");
    expect(harness.store.diagnostics().corruptions == 0, "a healthy read records no corruption");
}

// Bytes that are not the envelope at all: the magic check catches it before any field is trusted.
void a_foreign_file_reports_corrupted() {
    Harness harness;
    harness.files.files["/store/doc"] = "just some text, not an envelope at all";

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a foreign file does not load");
    expect(loaded.error().code == StorageErrorCode::Corrupted, "reported as Corrupted");
    expect(loaded.error().step == "header-magic", "the diagnostic names the magic check");

    const auto diagnostics = harness.store.diagnostics();
    expect(diagnostics.corruptions == 1, "the corruption is counted");
    expect(diagnostics.entries.size() == 1, "one diagnostic entry recorded");
    expect(diagnostics.entries[0].code == StorageErrorCode::Corrupted, "diagnostic code");
    expect(diagnostics.entries[0].entry == "doc", "the diagnostic names the entry");
    // The file is kept: a corrupt read must not silently destroy the user's data.
    expect(harness.files.has_file("/store/doc"), "the corrupt file is not deleted");
}

// A truncated file (the shape a torn write would have if the atomic sequence were not atomic): the
// declared length does not match the payload.
void a_truncated_entry_reports_corrupted() {
    Harness harness;
    harness.store.save("doc", "a longer payload that will be cut in half", {}, {});
    harness.run_one();
    const std::string path = "/store/doc";
    harness.files.files[path] = harness.files.files[path].substr(0, 30);

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a truncated entry does not load");
    expect(loaded.error().code == StorageErrorCode::Corrupted, "reported as Corrupted");
    expect(loaded.error().step == "header-length", "the length check caught it");
}

// One flipped payload byte: the length still matches, so only the checksum can catch it.
void a_bit_flip_reports_corrupted() {
    Harness harness;
    harness.store.save("doc", "payload", {}, {});
    harness.run_one();
    std::string& bytes = harness.files.files["/store/doc"];
    bytes.back() = static_cast<char>(bytes.back() ^ 0x01);

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a bit flip does not load");
    expect(loaded.error().code == StorageErrorCode::Corrupted, "reported as Corrupted");
    expect(loaded.error().step == "checksum", "the checksum caught it");
}

// A header shorter than the envelope: the truncation case a crash mid-write would leave.
void a_short_header_reports_corrupted() {
    Harness harness;
    harness.files.files["/store/doc"] = "TSR";

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a short header does not load");
    expect(loaded.error().step == "header-short", "the header length check caught it");
}

// A future format version is not guessed at: it is `Corrupted` too, so the caller falls back rather
// than mis-parsing a layout it does not know.
void a_future_version_reports_corrupted() {
    Harness harness;
    harness.store.save("doc", "payload", {}, {});
    harness.run_one();
    // Version lives at byte offset 4, little-endian u16.
    harness.files.files["/store/doc"][4] = static_cast<char>(9);

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("doc", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "an unknown version does not load");
    expect(loaded.error().step == "header-version", "the version check caught it");
}

// A settings document read as an opaque document (and vice versa) is caught by the kind field, so
// the two entry points cannot silently swap formats.
void a_settings_entry_read_as_a_document_reports_corrupted() {
    Harness harness;
    tessera::appfw::Settings values{{"theme", "dark"}};
    harness.store.save_settings("prefs", values, {}, {});
    harness.run_one();

    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("prefs", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a settings entry is not a document");
    expect(loaded.error().step == "header-kind", "the kind check caught it");
}

// A missing entry is `NotFound`, not `Corrupted`: the two drive different caller behaviour.
void a_missing_entry_is_not_found() {
    Harness harness;
    std::expected<tessera::appfw::StoredValue, StorageError> loaded;
    harness.store.load("absent", {}, [&](auto value) { loaded = std::move(value); });
    harness.run_one();

    expect(!loaded.has_value(), "a missing entry does not load");
    expect(loaded.error().code == StorageErrorCode::NotFound, "reported as NotFound");
    expect(harness.store.diagnostics().corruptions == 0, "a miss is not a corruption");
}

// The diagnostics ring is bounded: many corruptions keep the last N entries and count the rest, so
// a hostile file cannot grow memory without bound.
void the_diagnostics_ring_is_bounded() {
    StorageConfig config;
    config.root = "/store";
    config.diagnostic_capacity = 3;
    ManualIoExecutor executor;
    FakeFileSystem files;
    Storage store(config, executor, files);

    for (int index = 0; index < 6; ++index) {
        files.files["/store/doc" + std::to_string(index)] = "garbage";
        store.load("doc" + std::to_string(index), {}, {});
        executor.run(1);
        executor.run_completions(1);
    }

    const auto diagnostics = store.diagnostics();
    expect(diagnostics.recorded == 6, "all six corruptions are counted");
    expect(diagnostics.entries.size() == 3, "the ring keeps only its capacity");
    expect(diagnostics.dropped == 3, "the dropped count is reported");
    expect(diagnostics.corruptions == 6, "the corruption counter is exact");
}

} // namespace

int run_corruption_tests() {
    a_healthy_entry_round_trips();
    a_foreign_file_reports_corrupted();
    a_truncated_entry_reports_corrupted();
    a_bit_flip_reports_corrupted();
    a_short_header_reports_corrupted();
    a_future_version_reports_corrupted();
    a_settings_entry_read_as_a_document_reports_corrupted();
    a_missing_entry_is_not_found();
    the_diagnostics_ring_is_bounded();
    return g_failures;
}

} // namespace storage_test
