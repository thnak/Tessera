// SPDX-License-Identifier: MIT

#include "tessera/harness/artifact_sink.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>

namespace tessera::harness {
namespace {

ProtocolError internal_error(std::string message, std::string detail) {
    return ProtocolError{ProtocolErrorCode::Internal, std::move(message), std::move(detail),
                         std::nullopt};
}

bool is_allowed_path_byte(char value) noexcept {
    if (value >= 'a' && value <= 'z') {
        return true;
    }
    if (value >= 'A' && value <= 'Z') {
        return true;
    }
    if (value >= '0' && value <= '9') {
        return true;
    }
    return value == '.' || value == '_' || value == '-' || value == '/';
}

} // namespace

bool is_safe_artifact_path(std::string_view path) noexcept {
    if (path.empty() || path.size() > kMaxArtifactPathBytes) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return false;
    }
    // A drive-qualified or UNC path is absolute on Windows and must never be a script target.
    if (path.size() >= 2 && path[1] == ':') {
        return false;
    }
    for (const char value : path) {
        if (!is_allowed_path_byte(value)) {
            return false;
        }
    }
    // Every component must be a real name: no empty segment, no "." and no "..".
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = path.find('/', start);
        const std::string_view component = path.substr(
            start, slash == std::string_view::npos ? path.size() - start : slash - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            return true;
        }
        start = slash + 1;
    }
}

FileArtifactSink::FileArtifactSink(std::size_t max_bytes) noexcept
    : max_bytes_(max_bytes) {}

std::expected<void, ProtocolError> FileArtifactSink::write(std::string_view path,
                                                           std::string_view bytes) {
    if (!is_safe_artifact_path(path)) {
        return std::unexpected(ProtocolError{ProtocolErrorCode::InvalidArgument,
                                             "artifact path is not safe", std::string(path),
                                             std::nullopt});
    }
    if (bytes.size() > max_bytes_) {
        return std::unexpected(internal_error("artifact exceeds the size cap", std::string(path)));
    }
    const std::filesystem::path target{std::string(path)};
    if (!target.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error) {
            return std::unexpected(
                internal_error("failed to create the artifact directory", error.message()));
        }
    }
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return std::unexpected(
            internal_error("failed to open the artifact path", std::string(path)));
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) {
        return std::unexpected(internal_error("failed to write the artifact", std::string(path)));
    }
    return {};
}

} // namespace tessera::harness
