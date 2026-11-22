// SPDX-License-Identifier: MIT
//
// `FakeFileSystem`: the deterministic in-tree `hal::FileSystem` (issue #454). It ships in the
// library for the same reason `FakeNet` and `FakeShell` do: it fakes the module's *seam*, so a
// headless run and an app's own test use the same double the storage contract suite uses, and there
// is one definition of "what the file system did" instead of one per test file.
//
// **What it is for.** The atomic-replace sequence is deterministic, so a test does not hope for a
// mid-write failure: it asks for one. `fail_at()` names a single step, the sequence runs, and the
// assertions are about the exact call order and the bytes left behind. That is why every call is
// recorded in `calls()` in order, with the step name the `Storage` error would report.
//
// **What it is not.** It is not a POSIX emulation: `create_directory` records the path and does not
// create parents, paths are plain strings with `/` separators on every platform, and there is no
// locking, permissions or symlink behaviour. The one real-disk test in the suite exists precisely
// because this fake cannot answer "does the OS do what we asked".
//
// Determinism: no wall clock, no sleep, no thread, no `%TEMP%`. Two runs over the same script
// produce the same `calls()` and the same `files()`.

#pragma once

#include "tessera/hal/filesystem.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::appfw {

using hal::DirEntry;
using hal::FileError;
using hal::FileErrorCode;
using hal::FileReadResult;
using hal::FileResult;
using hal::FileSystem;

// Which call to fail, and with what. `None` means the fake behaves.
enum class FailStep : std::uint8_t {
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
struct FakeFileCall {
    std::string step;
    std::string path;

    friend bool operator==(const FakeFileCall&, const FakeFileCall&) = default;
};

class FakeFileSystem final : public FileSystem {
public:
    FakeFileSystem() = default;

    FakeFileSystem(const FakeFileSystem&) = delete;
    FakeFileSystem& operator=(const FakeFileSystem&) = delete;

    // --- scripting ---------------------------------------------------------------------------

    // The one step that fails, or `None`. Setting it back to `None` makes the fake behave again.
    void fail_at(FailStep step) noexcept;
    [[nodiscard]] FailStep failing_step() const noexcept;

    // True by default: a failed `write` leaves the bytes it managed to write, which is what a torn
    // write looks like. The sequence must remove that temp file, so a test asserts it did.
    void set_leave_partial_on_write_failure(bool leave) noexcept;

    // Called from inside `flush`, i.e. after the temp file was written and before the replace. This
    // is how a test cancels a write *mid-sequence* deterministically instead of racing it.
    void on_flush(std::function<void()> callback);

    // --- recorded state ----------------------------------------------------------------------

    [[nodiscard]] const std::map<std::string, std::string>& files() const noexcept;
    [[nodiscard]] const std::vector<std::string>& directories() const noexcept;
    // Every call, in order: "create-directory", "write", "flush", "flush-directory", "replace",
    // "remove", "read", "list".
    [[nodiscard]] const std::vector<FakeFileCall>& calls() const noexcept;
    [[nodiscard]] std::vector<std::string> steps() const;

    [[nodiscard]] bool has_file(std::string_view path) const;
    [[nodiscard]] std::string content_of(std::string_view path) const;
    // Temp files left behind, by the storage module's `.tmp` suffix. After a completed sequence
    // this is 0; the assertion that makes "no orphan temp file" a fact rather than an intention.
    [[nodiscard]] std::size_t temp_file_count() const;

    // Seeds a file without recording a call, so a test starts from an existing document instead of
    // writing one through the sequence under test.
    void put(std::string_view path, std::string_view content);
    void clear();

    // --- FileSystem ---------------------------------------------------------------------------

    [[nodiscard]] FileReadResult read(std::string_view path, std::size_t max_bytes) override;
    [[nodiscard]] FileResult write(std::string_view path, std::string_view data) override;
    [[nodiscard]] FileResult flush(std::string_view path) override;
    [[nodiscard]] FileResult flush_directory(std::string_view path) override;
    [[nodiscard]] FileResult replace(std::string_view from, std::string_view to) override;
    [[nodiscard]] FileResult remove(std::string_view path) override;
    [[nodiscard]] FileResult create_directory(std::string_view path) override;
    [[nodiscard]] bool exists(std::string_view path) override;
    [[nodiscard]] std::expected<std::uint64_t, FileError> size(std::string_view path) override;
    [[nodiscard]] std::expected<std::vector<DirEntry>, FileError>
    list(std::string_view directory) override;

private:
    [[nodiscard]] FileResult fail(FileErrorCode code, std::string_view step) const {
        return std::unexpected(FileError{code, step, std::int64_t{0}});
    }

    std::map<std::string, std::string> files_;
    std::vector<std::string> directories_;
    std::vector<FakeFileCall> calls_;
    std::function<void()> on_flush_;
    FailStep fail_ = FailStep::None;
    bool leave_partial_on_write_failure_ = true;
};

} // namespace tessera::appfw
