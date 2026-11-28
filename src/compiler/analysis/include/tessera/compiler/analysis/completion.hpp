// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Completions (issue #573, ADR-060, docs/specs/studio-services-v1.md section 13). The answer is a
// pure function of the project snapshot and the caret: same project, same caret, same list in the
// same order. Offsets are UTF-16 code units of the file text.
namespace tessera::compiler::analysis {

struct CompletionItem {
    std::string label; // what is inserted over [from, to)
    std::string kind;  // a symbol kind, or prop, attribute, cssClass, file, bind
    std::string detail;
};

// What the caret is in: tag (after `<`), attribute (inside an open tag), cssClass (`styles.`),
// importPath (inside the string of an import), bind (after `bind::` in a comment), expression, or
// none (a string, a comment, a member access that needs a type, an attribute value).
struct CompletionList {
    std::uint32_t from = 0; // the range the chosen label replaces: the word at the caret
    std::uint32_t to = 0;
    std::string context = "none";
    std::vector<CompletionItem> items;
    bool truncated = false; // more matches existed than kMaxCompletions
};

inline constexpr std::size_t kMaxCompletions = 200;

[[nodiscard]] CompletionList complete(const ProjectIndex& index, FileId file, std::uint32_t offset);
[[nodiscard]] std::string completion_to_json(const ProjectIndex& index, FileId file,
                                             std::uint32_t offset);

} // namespace tessera::compiler::analysis
