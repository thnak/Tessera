// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/symbol_index.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera::compiler::analysis {

struct SourceFile {
    FileId id = 0;
    std::string path; // project-relative, '/' separated; used only to resolve import specifiers
    std::string text; // UTF-8
};

// One replacement of a UTF-16 range of a file (docs/specs/studio-services-v1.md section 2).
struct TextEdit {
    FileId file = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::string new_text; // UTF-8
};

// What a position names: a declaration or a reference. `symbol` is empty for an unresolved
// reference.
struct Located {
    std::string symbol;
    bool declaration = false;
    std::optional<Reference> reference;
};

struct Occurrence {
    Span span;
    ReferenceKind kind = ReferenceKind::Value;
    bool declaration = false;
};

// The index of a project snapshot. Every per-file index is a pure function of that file's text, and
// project resolution is a pure function of the per-file indexes and the set of paths, so an
// incrementally updated index equals a fresh build of the same files (tested). Not thread safe: one
// instance per worker.
class ProjectIndex {
public:
    ProjectIndex() = default;

    [[nodiscard]] static ProjectIndex build(std::vector<SourceFile> files);

    // Adds or replaces a file and reindexes that file only.
    void set_file(SourceFile file);
    void remove_file(FileId file);
    // Applies the edits in order, each against the text the previous one produced (the page does
    // the same). False, with no change, when an edit names an unknown file or a range outside it.
    bool apply_edits(std::span<const TextEdit> edits);

    [[nodiscard]] const FileIndex* file(FileId file) const;
    [[nodiscard]] std::optional<std::string_view> text(FileId file) const;
    [[nodiscard]] const Symbol* symbol(std::string_view id) const;
    // Files in id order.
    [[nodiscard]] std::vector<FileId> files() const;

    [[nodiscard]] std::optional<Located> locate(FileId file, std::uint32_t offset) const;
    // Every use of a declaration, directly or through imports, plus the declaration itself when
    // `include_declaration`; sorted by file, then offset.
    [[nodiscard]] std::vector<Occurrence> references_to(std::string_view symbol,
                                                        bool include_declaration) const;

private:
    struct Entry {
        SourceFile source;
        FileIndex index;
    };

    void put_file(SourceFile file);
    void resolve_project();
    [[nodiscard]] const Symbol* exported(FileId file, std::string_view name, int depth) const;
    [[nodiscard]] std::optional<FileId> resolve_path(FileId from, std::string_view specifier) const;
    [[nodiscard]] const Symbol* follow(const Symbol* symbol) const;

    std::map<FileId, Entry> entries_;
    std::unordered_map<std::string, std::pair<FileId, std::size_t>> by_id_;
    std::unordered_map<std::string, FileId> paths_;
};

} // namespace tessera::compiler::analysis
