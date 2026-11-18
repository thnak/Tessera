// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_patch.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pieces the three destinations share (issue #171's `source.patch` slice). Each destination is
// its own translation unit because each one parses a different language - JSX, CSS, JSON - and the
// only thing they have in common is the edit list they produce.
namespace tessera::compiler::codegen::cli::internal {

// True when `offset` is a UTF-8 code point boundary of `text` (designer-protocol-v1 section 3.3:
// `offset` and `offset + length` are on code point boundaries). A continuation byte is `10xxxxxx`,
// so a position is a boundary unless the byte there is one. `offset == text.size()` is a boundary.
[[nodiscard]] bool is_code_point_boundary(std::string_view text, std::uint32_t offset);

// The failure the spec spells `component_invalid`: the range the edit would replace is not a
// boundary-respecting range of the file, so applying it would corrupt the text.
[[nodiscard]] std::unexpected<PatchError> invalid_range(std::string detail);

// The version the viewer holds for `file`, or `stale_source` when it holds none. A commit that
// would edit a file the viewer never staged cannot name a base version, and the host's comparison
// (section 10.3) would have nothing to compare.
[[nodiscard]] std::expected<std::uint32_t, PatchError> version_of(const PatchContext& context,
                                                                  std::string_view file);

// The text of a project file, or null when the project does not have it. The view points into the
// caller's `ProjectInput`, so it is valid exactly as long as that input is.
[[nodiscard]] std::string_view project_text(const ProjectInput& project, std::string_view path);

// The byte offset of the `>` or `/>` that closes the opening tag starting at `element_offset`, or 0
// when the tag has no end. The scan skips a quoted attribute value and a `{...}` expression, so a
// `>` inside either does not end the tag early. Used to insert an attribute the element does not
// have yet.
[[nodiscard]] std::uint32_t opening_tag_end(std::string_view text, std::uint32_t element_offset);

// The byte offset just past the end of the declaration starting at `declaration_offset`: the `;`
// that terminates it, or the `}` that closes its block when the last declaration has no semicolon.
// 0 when the declaration has no end.
[[nodiscard]] std::uint32_t declaration_end(std::string_view text,
                                            std::uint32_t declaration_offset);

// The byte offset of the `}` that closes the block of the rule starting at `rule_offset`, or 0 when
// the block is unterminated. A `}` inside a quoted string or a comment does not close it.
[[nodiscard]] std::uint32_t rule_block_end(std::string_view text, std::uint32_t rule_offset);

// Sorts `edits` by file and DESCENDING offset and rejects an overlap (designer-protocol-v1 section
// 10.2). Descending order is what makes "apply in order" safe: a later edit is always at a lower
// offset, so an earlier one can never have shifted it.
[[nodiscard]] std::expected<void, PatchError> order_edits(std::vector<TextEdit>& edits);

// One declaration the commit writes, in the order the override layer holds it.
using Declaration = std::pair<std::string, std::string>;

// The `inline` destination: the `style` attribute of the JSX element at `context.file`'s span.
[[nodiscard]] std::expected<std::vector<TextEdit>, PatchError>
inline_edits(const PatchRequest& request, const PatchContext& context);

// The `class` destination: the `.className` rule in `request.file`, plus the element's `className`
// attribute when it lists none.
[[nodiscard]] std::expected<std::vector<TextEdit>, PatchError>
class_edits(const PatchRequest& request, const PatchContext& context);

// The `token` destination: the value of the token declaration in the active theme's object.
[[nodiscard]] std::expected<std::vector<TextEdit>, PatchError>
token_edits(const PatchRequest& request, const PatchContext& context);

} // namespace tessera::compiler::codegen::cli::internal
