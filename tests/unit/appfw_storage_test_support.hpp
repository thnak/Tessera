// SPDX-License-Identifier: MIT
//
// Test fixtures for the AF7 storage contract suite (issue #179).
//
// The fake file system is the heart of the suite: it is an in-memory tree that records the exact
// order of the calls the atomic-replace sequence makes, and it can be told to fail at *one named
// step*. That is how the tests induce a mid-write failure instead of hoping for one: the sequence
// itself is deterministic, so "fail `replace`" is a reproducible experiment, not a race.
//
// Determinism: no wall clock, no sleep, no thread, no `%TEMP%`. The one real-file test writes under
// the build tree (`TESSERA_STORAGE_SCRATCH_DIR`).

#pragma once

#include "tessera/appfw/storage.hpp"
#include "tessera/hal/filesystem.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/manual_io_executor.hpp"

#include <cstddef>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace storage_test {

using tessera::appfw::Storage;
using tessera::appfw::StorageConfig;
using tessera::appfw::StorageError;
using tessera::appfw::StorageErrorCode;
using tessera::hal::DirEntry;
using tessera::hal::Durability;
using tessera::hal::FileError;
using tessera::hal::FileErrorCode;
using tessera::hal::FileReadResult;
using tessera::hal::FileResult;
using tessera::hal::FileSystem;
using tessera::runtime::CancellationSource;
using tessera::runtime::CancellationToken;
using tessera::runtime::ManualIoExecutor;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_storage: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// Which call to fail on, and with what. `None` means the fake behaves.
enum class FailStep {
    None,
    CreateDirectory,
    Write,
    Flush,
    FlushDirectory,
    Replace,
    Remove,
    Read,
    List,
};

// One recorded call. The step names are the ones `Storage` puts in a `StorageError`, so a test can
// assert the diagnostic and the call log agree.
struct Call {
    std::string step;
    std::string path;
};

class FakeFileSystem final : public FileSystem {
public:
    // --- configuration ---------------------------------------------------------------------
    FailStep fail = FailStep::None;
    // Set by `write` before it fails, to model a partial temp file left on disk by a torn write.
    bool leave_partial_on_write_failure = true;
    // Called from inside `flush`, i.e. after the temp file was written and before the replace. This
    // is how a test cancels a write *mid-sequence* deterministically instead of racing it.
    std::function<void()> on_flush;

    // --- recorded state --------------------------------------------------------------------
    std::map<std::string, std::string> files;
    std::vector<std::string> directories;
    // Every call, in order: "create-directory", "write", "flush", "flush-directory", "replace",
    // "remove", "read", "list".
    std::vector<Call> calls;

    [[nodiscard]] std::vector<std::string> steps() const {
        std::vector<std::string> names;
        names.reserve(calls.size());
        for (const Call& call : calls) {
            names.push_back(call.step);
        }
        return names;
    }

    [[nodiscard]] bool has_file(std::string_view path) const {
        return files.find(std::string(path)) != files.end();
    }

    [[nodiscard]] std::string content_of(std::string_view path) const {
        const auto found = files.find(std::string(path));
        return found == files.end() ? std::string() : found->second;
    }

    [[nodiscard]] std::size_t temp_file_count() const {
        std::size_t count = 0;
        for (const auto& [path, value] : files) {
            (void)value;
            if (path.size() > 4 && path.substr(path.size() - 4) == ".tmp") {
                ++count;
            }
        }
        return count;
    }

    // --- FileSystem -------------------------------------------------------------------------

    FileReadResult read(std::string_view path, std::size_t max_bytes) override {
        calls.push_back(Call{"read", std::string(path)});
        if (fail == FailStep::Read) {
            return std::unexpected(FileError{FileErrorCode::Io, "read", 0});
        }
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "read", 0});
        }
        if (found->second.size() > max_bytes) {
            return std::unexpected(FileError{FileErrorCode::TooLarge, "read", 0});
        }
        return found->second;
    }

    FileResult write(std::string_view path, std::string_view data) override {
        calls.push_back(Call{"write", std::string(path)});
        if (fail == FailStep::Write) {
            if (leave_partial_on_write_failure) {
                // Model a torn write: some bytes reached the file. The sequence must remove it.
                files[std::string(path)] = std::string(data.substr(0, data.size() / 2));
            }
            return std::unexpected(FileError{FileErrorCode::Io, "write", 0});
        }
        files[std::string(path)] = std::string(data);
        return {};
    }

    FileResult flush(std::string_view path) override {
        calls.push_back(Call{"flush", std::string(path)});
        if (on_flush) {
            on_flush();
        }
        if (fail == FailStep::Flush) {
            return std::unexpected(FileError{FileErrorCode::Io, "flush", 0});
        }
        if (files.find(std::string(path)) == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "flush", 0});
        }
        return {};
    }

    FileResult flush_directory(std::string_view path) override {
        calls.push_back(Call{"flush-directory", std::string(path)});
        if (fail == FailStep::FlushDirectory) {
            return std::unexpected(FileError{FileErrorCode::Io, "flush-dir", 0});
        }
        return {};
    }

    FileResult replace(std::string_view from, std::string_view to) override {
        calls.push_back(Call{"replace", std::string(from) + " -> " + std::string(to)});
        if (fail == FailStep::Replace) {
            return std::unexpected(FileError{FileErrorCode::Io, "replace", 0});
        }
        const auto source = files.find(std::string(from));
        if (source == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "replace", 0});
        }
        // Replace-over-existing, which is the contract: the destination's previous bytes are gone
        // and the source no longer exists.
        files[std::string(to)] = source->second;
        files.erase(source);
        return {};
    }

    FileResult remove(std::string_view path) override {
        calls.push_back(Call{"remove", std::string(path)});
        if (fail == FailStep::Remove) {
            return std::unexpected(FileError{FileErrorCode::Io, "remove", 0});
        }
        if (files.erase(std::string(path)) == 0) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "remove", 0});
        }
        return {};
    }

    FileResult create_directory(std::string_view path) override {
        calls.push_back(Call{"create-directory", std::string(path)});
        if (fail == FailStep::CreateDirectory) {
            return std::unexpected(FileError{FileErrorCode::Io, "create-directory", 0});
        }
        directories.emplace_back(path);
        return {};
    }

    bool exists(std::string_view path) override {
        return files.find(std::string(path)) != files.end();
    }

    std::expected<std::uint64_t, FileError> size(std::string_view path) override {
        const auto found = files.find(std::string(path));
        if (found == files.end()) {
            return std::unexpected(FileError{FileErrorCode::NotFound, "size", 0});
        }
        return static_cast<std::uint64_t>(found->second.size());
    }

    std::expected<std::vector<DirEntry>, FileError> list(std::string_view directory) override {
        calls.push_back(Call{"list", std::string(directory)});
        if (fail == FailStep::List) {
            return std::unexpected(FileError{FileErrorCode::Io, "list", 0});
        }
        std::vector<DirEntry> entries;
        const std::string prefix = std::string(directory) + "/";
        for (const auto& [path, value] : files) {
            if (path.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string name = path.substr(prefix.size());
            if (name.find('/') != std::string::npos) {
                continue;
            }
            entries.push_back(DirEntry{name, value.size(), true});
        }
        return entries;
    }
};

// The whole deterministic stack: a manual executor, a fake file system, a store and a queue that
// only exists because `WorkerIoExecutor` needs one. Tests drive it with `run()`.
struct Harness {
    explicit Harness(std::string root = "/store", std::size_t max_entry_bytes = 4096)
        : config{std::move(root), max_entry_bytes, Durability::BestEffort, 1, 8},
          store(config, executor, files) {}

    ManualIoExecutor executor;
    FakeFileSystem files;
    StorageConfig config;
    Storage store;

    // Runs one I/O body and then its completion, in the order the real executor delivers them. One
    // call = one whole operation, which is what a test wants most of the time.
    void run_one() {
        executor.run_one();
        executor.run_completions(1);
    }
    void run_all() {
        executor.run(64);
        executor.run_completions(64);
    }
};

} // namespace storage_test
