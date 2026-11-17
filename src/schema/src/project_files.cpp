// SPDX-License-Identifier: MIT

#include "tessera/schema/project_files.hpp"

#include <algorithm>
#include <array>

namespace tessera::schema {
namespace {

constexpr std::size_t kMaxPathBytes = 1024;

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// True when some segment before the last is a pruned directory name.
bool under_pruned_directory(std::string_view path) noexcept {
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = path.find('/', start);
        if (slash == std::string_view::npos) {
            return false;
        }
        if (is_pruned_directory(path.substr(start, slash - start))) {
            return true;
        }
        start = slash + 1;
    }
}

bool listed_as_asset(std::string_view path, std::span<const std::string> assets) noexcept {
    return std::any_of(assets.begin(), assets.end(), [path](const std::string& entry) {
        if (entry.empty()) {
            return false;
        }
        if (entry.back() == '/') {
            return path.size() > entry.size() && path.compare(0, entry.size(), entry) == 0;
        }
        return path == entry;
    });
}

ProjectFileError escape(std::string message) {
    return ProjectFileError{std::string(kPathEscapeCode), std::move(message)};
}

} // namespace

std::string_view to_string(ProjectFileKind kind) noexcept {
    switch (kind) {
    case ProjectFileKind::Source:
        return "source";
    case ProjectFileKind::Style:
        return "style";
    case ProjectFileKind::Config:
        return "config";
    case ProjectFileKind::Theme:
        return "theme";
    case ProjectFileKind::Asset:
        return "asset";
    case ProjectFileKind::Other:
        return "other";
    }
    return "other";
}

std::optional<ProjectFileError> check_project_path(std::string_view path) {
    if (path.empty()) {
        return escape("a project path must not be empty");
    }
    if (path.size() > kMaxPathBytes) {
        return escape("a project path is longer than 1024 bytes");
    }
    if (path.front() == '/' || (path.size() >= 2 && path[1] == ':')) {
        return escape("`" + std::string(path) + "` is absolute; a project path is relative");
    }
    if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
        return escape("`" + std::string(path) + "` holds a backslash or a NUL byte");
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment = path.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (segment.empty()) {
            return escape("`" + std::string(path) + "` has an empty segment");
        }
        if (segment == "." || segment == "..") {
            return escape("`" + std::string(path) + "` names `" + std::string(segment) +
                          "`, which leaves or hides the project root");
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return std::nullopt;
}

bool is_pruned_directory(std::string_view name) noexcept {
    return name == "node_modules" || (!name.empty() && name.front() == '.');
}

ProjectFileKind classify_project_file(std::string_view path, const ProjectFileContext& context) {
    if (under_pruned_directory(path)) {
        return ProjectFileKind::Other;
    }
    if (path == "tessera.json") {
        return ProjectFileKind::Config;
    }
    if (!context.theme.empty() && path == context.theme) {
        return ProjectFileKind::Theme;
    }
    if (ends_with(path, ".tsx") || ends_with(path, ".jsx")) {
        return ProjectFileKind::Source;
    }
    if (ends_with(path, ".css")) {
        return ProjectFileKind::Style;
    }
    if (listed_as_asset(path, context.assets)) {
        return ProjectFileKind::Asset;
    }
    return ProjectFileKind::Other;
}

std::optional<ProjectFileError> check_project_limits(std::span<const ProjectFileEntry> entries) {
    if (entries.size() > kMaxProjectFiles) {
        return ProjectFileError{std::string(kFileLimitCode),
                                "the project has " + std::to_string(entries.size()) +
                                    " files; the limit is " + std::to_string(kMaxProjectFiles)};
    }
    std::uint64_t total = 0;
    for (const ProjectFileEntry& entry : entries) {
        if (!is_readable(entry.kind)) {
            continue;
        }
        if (entry.size > kMaxProjectFileBytes) {
            return ProjectFileError{std::string(kFileLimitCode),
                                    "`" + entry.path + "` is " + std::to_string(entry.size) +
                                        " bytes; the per-file limit is " +
                                        std::to_string(kMaxProjectFileBytes)};
        }
        total += entry.size;
        if (total > kMaxProjectBytes) {
            return ProjectFileError{std::string(kFileLimitCode),
                                    "the readable files pass the total limit of " +
                                        std::to_string(kMaxProjectBytes) + " bytes at `" +
                                        entry.path + "`"};
        }
    }
    return std::nullopt;
}

std::string project_file_digest(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    constexpr std::array<char, 16> kDigits = {'0', '1', '2', '3', '4', '5', '6', '7',
                                              '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out = "fnv1a64:";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out.push_back(kDigits[(hash >> shift) & 0xF]);
    }
    return out;
}

} // namespace tessera::schema
