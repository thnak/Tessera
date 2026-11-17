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

std::string_view to_string(RootSource source) noexcept {
    switch (source) {
    case RootSource::Argument:
        return "argument";
    case RootSource::Roots:
        return "roots";
    case RootSource::Cwd:
        return "cwd";
    }
    return "cwd";
}

namespace {

// Builds the record for an existing directory, or nullopt when its path does not normalize.
std::optional<OpenProject> make_project(const fs::path& directory, std::string echo,
                                        RootSource source) {
    std::error_code error;
    fs::path absolute = fs::absolute(directory, error);
    if (error) {
        absolute = directory;
    }
    absolute = absolute.lexically_normal();
    OpenProject project;
    project.path = std::move(echo);
    project.root = normalize_project_path(absolute.generic_string());
    if (project.root.empty()) {
        return std::nullopt;
    }
    project.directory = absolute;
    // `lexically_normal` leaves a trailing separator as an empty filename, so step over it.
    const fs::path name =
        absolute.has_filename() ? absolute.filename() : absolute.parent_path().filename();
    project.folder = name.empty() ? std::string("(root)") : name.generic_string();
    project.source = source;
    return project;
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string path_from_file_uri(std::string_view uri) {
    constexpr std::string_view kScheme = "file://";
    if (uri.substr(0, kScheme.size()) != kScheme) {
        return {};
    }
    std::string_view rest = uri.substr(kScheme.size());
    // `file:///path` has an empty authority; `file://localhost/path` names this machine. Any other
    // host is a remote path this server must not touch.
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        return {};
    }
    const std::string_view authority = rest.substr(0, slash);
    if (!authority.empty() && authority != "localhost") {
        return {};
    }
    rest = rest.substr(slash);
    std::string path;
    for (std::size_t index = 0; index < rest.size(); ++index) {
        if (rest[index] != '%') {
            path.push_back(rest[index]);
            continue;
        }
        if (index + 2 >= rest.size()) {
            return {};
        }
        const int high = hex_value(rest[index + 1]);
        const int low = hex_value(rest[index + 2]);
        if (high < 0 || low < 0) {
            return {};
        }
        const char decoded = static_cast<char>(high * 16 + low);
        if (decoded == '\0') {
            return {};
        }
        path.push_back(decoded);
        index += 2;
    }
    // A Windows drive path arrives as `/C:/Users/x`: drop the slash so it is a real path.
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':' &&
        ((path[1] >= 'A' && path[1] <= 'Z') || (path[1] >= 'a' && path[1] <= 'z'))) {
        path.erase(0, 1);
    }
    return path;
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
    std::optional<OpenProject> project = make_project(directory, path, RootSource::Argument);
    if (!project) {
        return "open_project: `" + path + "` does not normalize to a project path";
    }
    project_ = std::move(project);
    return std::nullopt;
}

void SessionState::apply_roots(const std::vector<std::string>& uris) {
    if (project_ && project_->source == RootSource::Argument) {
        return; // an explicit open_project overrides the client's roots
    }
    for (const std::string& uri : uris) {
        const std::string path = path_from_file_uri(uri);
        if (path.empty()) {
            continue;
        }
        std::error_code error;
        if (!fs::is_directory(fs::path(path), error)) {
            continue;
        }
        if (std::optional<OpenProject> project =
                make_project(fs::path(path), {}, RootSource::Roots)) {
            project_ = std::move(project);
            return;
        }
    }
    // No usable root: forget a previous roots answer so the working directory applies again.
    if (project_ && project_->source == RootSource::Roots) {
        project_.reset();
    }
}

const OpenProject* SessionState::resolved_project() {
    if (!project_) {
        std::error_code error;
        const fs::path directory = fs::current_path(error);
        if (error) {
            return nullptr;
        }
        project_ = make_project(directory, {}, RootSource::Cwd);
    }
    return project_ ? &*project_ : nullptr;
}

} // namespace tessera::mcp
