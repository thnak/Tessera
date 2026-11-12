// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tessera::ui::style {

// Serialized prepared stylesheet (ADR-048, docs/specs/compiled-stylesheet-v1.md). The bytes are
// deterministic: identical prepared data always serializes to identical bytes.
inline constexpr std::uint32_t kStylesheetBlobFormatVersion = 1;
// Bumped whenever a change to the engine alters what prepared data means (selector forms, property
// expansion, supported at-rules) without changing the layout of the blob.
inline constexpr std::uint32_t kStylesheetEngineSchemaVersion = 1;

enum class BlobErrorCode : std::uint8_t {
    Truncated,
    BadMagic,
    UnsupportedFormatVersion,
    EngineSchemaMismatch,
    HashMismatch,
    Corrupt,
};

struct BlobError {
    BlobErrorCode code = BlobErrorCode::Corrupt;
    std::string message;
};

[[nodiscard]] std::vector<std::uint8_t> serialize_stylesheet(const PreparedStyleSheet& prepared);

// Validates the header, bounds and every index before returning; never reads outside `bytes` and
// never throws on malformed input. When `expected_hash` is set the blob's source hash must match.
[[nodiscard]] std::expected<PreparedStyleSheet, BlobError>
load_stylesheet_blob(std::span<const std::uint8_t> bytes,
                     std::optional<std::uint64_t> expected_hash = std::nullopt);

} // namespace tessera::ui::style
