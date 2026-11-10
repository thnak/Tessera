// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/harness/automation_host.hpp"

#include <cstddef>
#include <expected>
#include <string_view>

namespace tessera::harness {

// Artifact paths come from untrusted script input (spec section 9). A path is safe when it is a
// relative, '/'-separated path below the working directory, made only of [A-Za-z0-9._-] segments,
// with no empty, "." or ".." component. Backslashes are rejected rather than normalized so a
// Windows client and a Linux client cannot disagree about what `..\\x` means, and drive/UNC
// prefixes can never be written.
inline constexpr std::size_t kMaxArtifactPathBytes = 512;

[[nodiscard]] bool is_safe_artifact_path(std::string_view path) noexcept;

// Where a trace or audit document goes. The handler enforces the byte cap before calling this, so
// a fake sink in tests never touches the filesystem.
class ArtifactSink {
public:
    ArtifactSink() = default;
    ArtifactSink(const ArtifactSink&) = delete;
    ArtifactSink& operator=(const ArtifactSink&) = delete;
    virtual ~ArtifactSink() = default;

    [[nodiscard]] virtual std::expected<void, ProtocolError> write(std::string_view path,
                                                                   std::string_view bytes) = 0;
};

// Writes UTF-8 bytes to a relative path, creating intermediate directories. The sink re-checks
// the path and the cap itself: a caller that skips validation still cannot escape the working
// directory or write an unbounded document.
class FileArtifactSink final : public ArtifactSink {
public:
    explicit FileArtifactSink(std::size_t max_bytes = kMaxArtifactBytes) noexcept;

    [[nodiscard]] std::expected<void, ProtocolError> write(std::string_view path,
                                                           std::string_view bytes) override;

private:
    std::size_t max_bytes_;
};

} // namespace tessera::harness
