// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/limits.hpp"
#include "tool_session.hpp"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace tessera::mcp {
namespace {

namespace fs = std::filesystem;

// Splits on both separators so one normalization covers a Windows and a POSIX spelling of the same
// project-relative path, which is what a client on either host may send.
std::vector<std::string> segments(std::string_view path) {
    std::vector<std::string> parts;
    std::string current;
    for (const char character : path) {
        if (character == '/' || character == '\\') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        current.push_back(character);
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    return parts;
}

} // namespace

std::string normalize_project_path(std::string_view path) {
    std::string normalized;
    for (const std::string& part : segments(path)) {
        if (part == ".") {
            continue;
        }
        if (part == "..") {
            // A span path that climbs out of the project cannot be matched to a project file, so it
            // is reported as unnameable instead of being resolved to some host directory.
            return {};
        }
        if (!normalized.empty()) {
            normalized.push_back('/');
        }
        normalized.append(part);
    }
    return normalized;
}

std::optional<std::string> SessionState::open_project(std::string path) {
    if (path.empty()) {
        return "open_project: `path` must not be empty";
    }
    std::error_code error;
    const fs::path directory(path);
    if (!fs::is_directory(directory, error)) {
        return "open_project: `" + path + "` is not a readable directory";
    }
    fs::path absolute = fs::absolute(directory, error);
    if (error) {
        absolute = directory;
    }
    OpenProject project;
    project.path = std::move(path);
    project.root = normalize_project_path(absolute.lexically_normal().generic_string());
    if (project.root.empty()) {
        return "open_project: `" + project.path + "` does not normalize to a project path";
    }
    project_ = std::move(project);
    return std::nullopt;
}

} // namespace tessera::mcp
