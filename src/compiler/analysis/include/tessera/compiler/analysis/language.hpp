// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/project_index.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Definition, references and hover (issues #571 and #572, ADR-060, docs/specs/studio-services-v1.md
// section 12). Every answer comes from the symbol index; offsets are UTF-16 code units of the file
// text, like the rest of the analysis protocol.
namespace tessera::compiler::analysis {

// Where the name under the caret is declared. An import binding is followed to the declaration it
// stands for (through `export { X } from` chains); an import that does not resolve (a package, a
// missing file) answers with the binding itself. Empty `spans` carries the reason in `reason`
// (None when the caret is on no name at all).
struct Definition {
    std::vector<Span> spans;
    Unresolved reason = Unresolved::None;
};

struct ReferenceList {
    std::string
        symbol; // the declaration's id (an imported name resolves to its origin); empty if none
    std::string name;
    std::vector<Occurrence> occurrences;
    Unresolved reason = Unresolved::None;
};

struct Hover {
    Span range;       // the name under the caret
    std::string kind; // a symbol kind, or the reference kind for an unresolved name
    std::string name;
    std::string signature; // the first line of the declaration, trimmed; empty for props/params
    std::string doc;       // the comment block directly above the declaration
    std::string container; // the enclosing component or hook name
    bool resolved = false;
    bool exported = false;
    std::optional<Span> declaration;
    Unresolved reason = Unresolved::None;
    std::vector<Diagnostic> diagnostics; // those of the file that cover the caret
};

[[nodiscard]] Definition definition(const ProjectIndex& index, FileId file, std::uint32_t offset);
[[nodiscard]] ReferenceList find_references(const ProjectIndex& index, FileId file,
                                            std::uint32_t offset, bool include_declaration);
// Empty when the caret names nothing and no diagnostic covers it.
[[nodiscard]] std::optional<Hover> hover(const ProjectIndex& index, FileId file,
                                         std::uint32_t offset);

// The responses of the `definition`, `references` and `hover` requests as deterministic JSON text
// (fixed member order, no whitespace).
[[nodiscard]] std::string definition_to_json(const ProjectIndex& index, FileId file,
                                             std::uint32_t offset);
[[nodiscard]] std::string references_to_json(const ProjectIndex& index, FileId file,
                                             std::uint32_t offset, bool include_declaration);
[[nodiscard]] std::string hover_to_json(const ProjectIndex& index, FileId file,
                                        std::uint32_t offset);

} // namespace tessera::compiler::analysis
