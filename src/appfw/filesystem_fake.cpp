// SPDX-License-Identifier: MIT

#include "tessera/appfw/filesystem_fake.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace tessera::appfw {

void FakeFileSystem::fail_at(FailStep step) noexcept {
    fail_ = step;
}

FailStep FakeFileSystem::failing_step() const noexcept {
    return fail_;
}

void FakeFileSystem::set_leave_partial_on_write_failure(bool leave) noexcept {
    leave_partial_on_write_failure_ = leave;
}

void FakeFileSystem::on_flush(std::function<void()> callback) {
    on_flush_ = std::move(callback);
}

const std::map<std::string, std::string>& FakeFileSystem::files() const noexcept {
    return files_;
}

const std::vector<std::string>& FakeFileSystem::directories() const noexcept {
    return directories_;
}

const std::vector<FakeFileCall>& FakeFileSystem::calls() const noexcept {
    return calls_;
}

std::vector<std::string> FakeFileSystem::steps() const {
    std::vector<std::string> names;
    names.reserve(calls_.size());
    for (const FakeFileCall& call : calls_) {
        names.push_back(call.step);
    }
    return names;
}

bool FakeFileSystem::has_file(std::string_view path) const {
    return files_.contains(std::string(path));
}

std::string FakeFileSystem::content_of(std::string_view path) const {
    const auto found = files_.find(std::string(path));
    return found == files_.end() ? std::string() : found->second;
}

std::size_t FakeFileSystem::temp_file_count() const {
    std::size_t count = 0;
    for (const auto& [path, value] : files_) {
        (void)value;
        if (path.ends_with(".tmp")) {
            ++count;
        }
    }
    return count;
}

void FakeFileSystem::put(std::string_view path, std::string_view content) {
    files_[std::string(path)] = std::string(content);
}

void FakeFileSystem::clear() {
    files_.clear();
    directories_.clear();
    calls_.clear();
}

FileReadResult FakeFileSystem::read(std::string_view path, std::size_t max_bytes) {
    calls_.push_back(FakeFileCall{"read", std::string(path)});
    if (fail_ == FailStep::Read) {
        return std::unexpected(FileError{FileErrorCode::Io, "read", 0});
    }
    const auto found = files_.find(std::string(path));
    if (found == files_.end()) {
        return std::unexpected(FileError{FileErrorCode::NotFound, "read", 0});
    }
    if (found->second.size() > max_bytes) {
        return std::unexpected(FileError{FileErrorCode::TooLarge, "read", 0});
    }
    return found->second;
}

FileResult FakeFileSystem::write(std::string_view path, std::string_view data) {
    calls_.push_back(FakeFileCall{"write", std::string(path)});
    if (fail_ == FailStep::Write) {
        if (leave_partial_on_write_failure_) {
            // Model a torn write: some bytes reached the file. The sequence must remove it.
            files_[std::string(path)] = std::string(data.substr(0, data.size() / 2));
        }
        return fail(FileErrorCode::Io, "write");
    }
    files_[std::string(path)] = std::string(data);
    return {};
}

FileResult FakeFileSystem::flush(std::string_view path) {
    calls_.push_back(FakeFileCall{"flush", std::string(path)});
    if (on_flush_) {
        on_flush_();
    }
    if (fail_ == FailStep::Flush) {
        return fail(FileErrorCode::Io, "flush");
    }
    if (!files_.contains(std::string(path))) {
        return fail(FileErrorCode::NotFound, "flush");
    }
    return {};
}

FileResult FakeFileSystem::flush_directory(std::string_view path) {
    calls_.push_back(FakeFileCall{"flush-directory", std::string(path)});
    if (fail_ == FailStep::FlushDirectory) {
        return fail(FileErrorCode::Io, "flush-dir");
    }
    return {};
}

FileResult FakeFileSystem::replace(std::string_view from, std::string_view to) {
    calls_.push_back(FakeFileCall{"replace", std::string(from) + " -> " + std::string(to)});
    if (fail_ == FailStep::Replace) {
        return fail(FileErrorCode::Io, "replace");
    }
    // A self-replace is a no-op success, not a delete. The storage sequence never asks for one (its
    // temp file is always a distinct path), but a caller that does would otherwise lose the file to
    // the erase below, which is the kind of fake-only bug that makes a test disagree with the OS.
    if (from == to) {
        return !files_.contains(std::string(from)) ? fail(FileErrorCode::NotFound, "replace")
                                                   : FileResult{};
    }
    const auto source = files_.find(std::string(from));
    if (source == files_.end()) {
        return fail(FileErrorCode::NotFound, "replace");
    }
    // Replace-over-existing, which is the contract: the destination's previous bytes are gone and
    // the source no longer exists.
    files_[std::string(to)] = source->second;
    files_.erase(source);
    return {};
}

FileResult FakeFileSystem::remove(std::string_view path) {
    calls_.push_back(FakeFileCall{"remove", std::string(path)});
    if (fail_ == FailStep::Remove) {
        return fail(FileErrorCode::Io, "remove");
    }
    if (files_.erase(std::string(path)) == 0) {
        return fail(FileErrorCode::NotFound, "remove");
    }
    return {};
}

FileResult FakeFileSystem::create_directory(std::string_view path) {
    calls_.push_back(FakeFileCall{"create-directory", std::string(path)});
    if (fail_ == FailStep::CreateDirectory) {
        return fail(FileErrorCode::Io, "create-directory");
    }
    directories_.emplace_back(path);
    return {};
}

bool FakeFileSystem::exists(std::string_view path) {
    return files_.contains(std::string(path));
}

std::expected<std::uint64_t, FileError> FakeFileSystem::size(std::string_view path) {
    const auto found = files_.find(std::string(path));
    if (found == files_.end()) {
        return std::unexpected(FileError{FileErrorCode::NotFound, "size", 0});
    }
    return static_cast<std::uint64_t>(found->second.size());
}

std::expected<std::vector<DirEntry>, FileError> FakeFileSystem::list(std::string_view directory) {
    calls_.push_back(FakeFileCall{"list", std::string(directory)});
    if (fail_ == FailStep::List) {
        return std::unexpected(FileError{FileErrorCode::Io, "list", 0});
    }
    std::vector<DirEntry> entries;
    const std::string prefix = std::string(directory) + "/";
    for (const auto& [path, value] : files_) {
        if (!path.starts_with(prefix)) {
            continue;
        }
        const std::string name = path.substr(prefix.size());
        if (name.find('/') != std::string::npos) {
            continue;
        }
        entries.push_back(DirEntry{name, value.size(), true});
    }
    // Sorted by name, which the `FileSystem` contract requires and a `std::map` of full paths does
    // not give for free once the prefix is stripped.
    std::ranges::sort(entries, {}, &DirEntry::name);
    return entries;
}

} // namespace tessera::appfw
