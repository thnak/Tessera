// SPDX-License-Identifier: MIT

// The shared half of `source.patch` (issue #171, designer-protocol-v1 section 10.2): the UTF-8
// boundary rule, the base-version lookup, and the ordering invariant every destination's edit list
// must satisfy.

#include "scene_canvas_patch_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {

std::expected<SourcePatch, PatchError> build_source_patch(const PatchRequest& request,
                                                          const PatchContext& context) {
    // The order of these two checks is the spec's: a NodeRef that names no node is `no_such_node`
    // whatever it carries, and a node with no overrides is `no_overrides` whatever the destination.
    // Answering the destination's failure first would tell a page to fix a class file when the real
    // problem is that the node is gone.
    if (!context.node_found) {
        return std::unexpected(
            PatchError{PatchErrorCode::NoSuchNode, "the node is not in the tree"});
    }
    if (request.declarations.empty()) {
        return std::unexpected(
            PatchError{PatchErrorCode::NoOverrides, "the node has no overrides to commit"});
    }

    std::expected<std::vector<TextEdit>, PatchError> edits =
        std::unexpected(PatchError{PatchErrorCode::ComponentInvalid, "unknown destination"});
    switch (request.destination) {
    case PatchDestination::Inline:
        edits = internal::inline_edits(request, context);
        break;
    case PatchDestination::Class:
        edits = internal::class_edits(request, context);
        break;
    case PatchDestination::Token:
        edits = internal::token_edits(request, context);
        break;
    }
    if (!edits) {
        return std::unexpected(edits.error());
    }

    SourcePatch patch;
    patch.edits = std::move(*edits);
    if (const auto ordered = internal::order_edits(patch.edits); !ordered) {
        return std::unexpected(ordered.error());
    }
    // One base version per affected file, in the same order the edits name them, so a host can
    // compare every file before applying any of them (section 10.3: the patch is atomic).
    for (const TextEdit& edit : patch.edits) {
        const bool known = std::any_of(patch.base_versions.begin(), patch.base_versions.end(),
                                       [&](const std::pair<std::string, std::uint32_t>& entry) {
                                           return entry.first == edit.file;
                                       });
        if (known) {
            continue;
        }
        const auto version = internal::version_of(context, edit.file);
        if (!version) {
            return std::unexpected(version.error());
        }
        patch.base_versions.emplace_back(edit.file, *version);
    }
    return patch;
}

} // namespace tessera::compiler::codegen::cli

namespace tessera::compiler::codegen::cli::internal {

bool is_code_point_boundary(std::string_view text, std::uint32_t offset) {
    if (offset > text.size()) {
        return false;
    }
    if (offset == text.size()) {
        return true;
    }
    // A continuation byte is `10xxxxxx`. A position that lands on one is inside a code point, so an
    // edit there would split a character and corrupt the file.
    return (static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U;
}

std::unexpected<PatchError> invalid_range(std::string detail) {
    return std::unexpected(PatchError{PatchErrorCode::ComponentInvalid, std::move(detail)});
}

std::expected<std::uint32_t, PatchError> version_of(const PatchContext& context,
                                                    std::string_view file) {
    for (const auto& [path, version] : context.versions) {
        if (path == file) {
            return version;
        }
    }
    return std::unexpected(PatchError{
        PatchErrorCode::StaleSource, "the viewer holds no version of `" + std::string(file) + "`"});
}

std::string_view project_text(const ProjectInput& project, std::string_view path) {
    for (const ProjectFile& file : project.sources) {
        if (file.path == path) {
            return file.text;
        }
    }
    for (const ProjectFile& file : project.stylesheets) {
        if (file.path == path) {
            return file.text;
        }
    }
    return {};
}

std::uint32_t declaration_end(std::string_view text, std::uint32_t declaration_offset) {
    std::size_t index = declaration_offset;
    while (index < text.size()) {
        const char c = text[index];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++index;
            while (index < text.size() && text[index] != quote) {
                if (text[index] == '\\') {
                    ++index;
                }
                ++index;
            }
            if (index < text.size()) {
                ++index;
            }
            continue;
        }
        if (c == '/' && index + 1 < text.size() && text[index + 1] == '*') {
            index += 2;
            while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/')) {
                ++index;
            }
            index = index + 1 < text.size() ? index + 2 : text.size();
            continue;
        }
        if (c == ';') {
            // Past the semicolon: the edit replaces the declaration and its terminator, so the
            // replacement's own `;` is the one that survives.
            return static_cast<std::uint32_t>(index + 1);
        }
        if (c == '}') {
            // The last declaration of a block may omit its semicolon; the `}` is not part of it.
            return static_cast<std::uint32_t>(index);
        }
        ++index;
    }
    return 0;
}

std::uint32_t rule_block_end(std::string_view text, std::uint32_t rule_offset) {
    std::size_t index = rule_offset;
    // Find the `{` that opens the block, skipping a quoted string in the selector.
    while (index < text.size()) {
        const char c = text[index];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++index;
            while (index < text.size() && text[index] != quote) {
                if (text[index] == '\\') {
                    ++index;
                }
                ++index;
            }
            if (index < text.size()) {
                ++index;
            }
            continue;
        }
        if (c == '{') {
            break;
        }
        ++index;
    }
    if (index >= text.size()) {
        return 0;
    }
    int depth = 1;
    ++index;
    while (index < text.size()) {
        const char c = text[index];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++index;
            while (index < text.size() && text[index] != quote) {
                if (text[index] == '\\') {
                    ++index;
                }
                ++index;
            }
            if (index < text.size()) {
                ++index;
            }
            continue;
        }
        if (c == '/' && index + 1 < text.size() && text[index + 1] == '*') {
            index += 2;
            while (index + 1 < text.size() && !(text[index] == '*' && text[index + 1] == '/')) {
                ++index;
            }
            index = index + 1 < text.size() ? index + 2 : text.size();
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return static_cast<std::uint32_t>(index);
            }
        }
        ++index;
    }
    return 0;
}

std::uint32_t opening_tag_end(std::string_view text, std::uint32_t element_offset) {
    if (element_offset >= text.size() || text[element_offset] != '<') {
        return 0;
    }
    // The scan starts past `<` and the tag name. A quoted value and a `{...}` expression are the
    // two places a `>` can appear without ending the tag, so both are skipped whole; a `{` inside a
    // quoted value is just a character, and a quote inside an expression is just a character, which
    // is why the two states are exclusive rather than nested.
    std::size_t index = element_offset + 1;
    while (index < text.size()) {
        const char c = text[index];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++index;
            while (index < text.size() && text[index] != quote) {
                if (text[index] == '\\') {
                    ++index;
                }
                ++index;
            }
            if (index < text.size()) {
                ++index;
            }
            continue;
        }
        if (c == '{') {
            int depth = 1;
            ++index;
            while (index < text.size() && depth > 0) {
                if (text[index] == '{') {
                    ++depth;
                } else if (text[index] == '}') {
                    --depth;
                }
                ++index;
            }
            continue;
        }
        if (c == '>') {
            return static_cast<std::uint32_t>(index);
        }
        ++index;
    }
    return 0;
}

std::expected<void, PatchError> order_edits(std::vector<TextEdit>& edits) {
    std::sort(edits.begin(), edits.end(), [](const TextEdit& left, const TextEdit& right) {
        if (left.file != right.file) {
            return left.file < right.file;
        }
        // Descending offset: applying the list in order never shifts a later edit, because every
        // later edit is at a lower offset than the one before it.
        return left.offset > right.offset;
    });
    for (std::size_t index = 1; index < edits.size(); ++index) {
        const TextEdit& previous = edits[index - 1];
        const TextEdit& current = edits[index];
        if (previous.file != current.file) {
            continue;
        }
        // Descending order means the previous edit is at a higher offset; the two overlap when the
        // current one's end reaches past the previous one's start. An insertion (`length == 0`) at
        // the same offset as a replacement is an overlap too: which of the two came first would
        // decide the result, and the spec forbids that ambiguity.
        const std::uint64_t previous_start = previous.offset;
        const std::uint64_t current_end =
            static_cast<std::uint64_t>(current.offset) + current.length;
        if (current_end > previous_start ||
            (current.length == 0 && current.offset == previous.offset)) {
            return std::unexpected(PatchError{PatchErrorCode::ComponentInvalid,
                                              "two edits of `" + current.file + "` overlap"});
        }
    }
    return {};
}

} // namespace tessera::compiler::codegen::cli::internal
