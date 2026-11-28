// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Prepare-rename and rename across files (issue #574, ADR-060, docs/specs/studio-services-v1.md
// section 13). A rename answers a multi-file list of UTF-16 TextEdits that never overlap, or
// refuses with a code and a message and no edits: nothing is half-renamed.
namespace tessera::compiler::analysis {

struct Refusal {
    std::string code; // TESSERA_ANALYSIS_RENAME_*
    std::string message;
};

struct PrepareRename {
    bool ok = false;
    Span range;              // the name under the caret
    std::string placeholder; // the current name
    std::string kind;        // the declaration's symbol kind, or cssClass
    std::optional<Refusal> refusal;
};

struct RenameResult {
    // Sorted by file id, then offset descending, so applying them in order (each against the
    // text the previous one produced) and applying them all against the original text agree.
    std::vector<TextEdit> edits;
    std::optional<Refusal> refusal;
};

[[nodiscard]] PrepareRename prepare_rename(const ProjectIndex& index, FileId file,
                                           std::uint32_t offset);
[[nodiscard]] RenameResult rename_symbol(const ProjectIndex& index, FileId file,
                                         std::uint32_t offset, std::string_view new_name);

[[nodiscard]] std::string prepare_rename_to_json(const ProjectIndex& index, FileId file,
                                                 std::uint32_t offset);
[[nodiscard]] std::string rename_to_json(const ProjectIndex& index, FileId file,
                                         std::uint32_t offset, std::string_view new_name);

} // namespace tessera::compiler::analysis
