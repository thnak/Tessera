// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

// One embedded stylesheet as codegen emits it (docs/specs/compiled-stylesheet-v1.md section 9):
// the project path, used by diagnostics, and the blob bytes. `<module>_stylesheets.hpp` includes
// this header and builds its registration table from this type, so the engine and the generated
// table cannot drift apart.
struct EmbeddedStylesheet {
    std::string_view path;
    std::span<const std::uint8_t> bytes;
};

// Serialized prepared stylesheet (ADR-048, docs/specs/compiled-stylesheet-v1.md). The bytes are
// deterministic: identical prepared data always serializes to identical bytes.
// 2: a rule carries its enclosing `@scope` regions (issue #622), so the item body gained a field.
inline constexpr std::uint32_t kStylesheetBlobFormatVersion = 2;
// Bumped whenever a change to the engine alters what prepared data means (selector forms, property
// expansion, supported at-rules) without changing the layout of the blob. 2: `::part()` selectors
// now match a part element across the component boundary (issue #235), so a blob that carries one
// means something different to the engine that wrote it under 1. 3: the CSS Grid longhands are
// accepted and `display: grid` lays out as a grid rather than a flex container (issue #249), so the
// same declarations mean something different to an engine that wrote them under 2. 4: `:has()` is
// matched and `@scope` is evaluated (issue #622), so a selector or at-rule a blob carries now has a
// meaning it did not have under 3.
inline constexpr std::uint32_t kStylesheetEngineSchemaVersion = 4;

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
