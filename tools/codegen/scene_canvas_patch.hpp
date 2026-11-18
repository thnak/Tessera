// SPDX-License-Identifier: MIT

#pragma once

#include "scene_project.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// `source.patch` (designer-protocol-v1 section 10.2): turning a style commit into the minimal list
// of text edits a host applies.
//
// **What this file is, and what it is not.** It is the *viewer's* half: given a node and a
// destination, it produces the edits and the base versions. It never writes a file, never reads one
// from disk, and never sends anything: the host applies the edits and answers `source.patch.result`
// (section 10.3), which is the studio's project-IO path and a separate concern. The bytes it edits
// are the ones the viewer already staged, so an edit is computed against exactly the text the
// offsets were measured in.
//
// **Why the edits are computed here and not in the page.** Every range is a byte offset into a
// project file, and the only code that knows where a JSX attribute or a CSS rule sits is the code
// that parsed it. A page that searched the text for `style=` would be a second, weaker parser, and
// the first time the two disagreed it would corrupt a developer's file - the worst failure this
// slice can have.
namespace tessera::compiler::codegen::cli {

// One replacement of a byte range of a file by new text (designer-protocol-v1 section 3.3).
// `offset` and `offset + length` are on UTF-8 code point boundaries; `length` 0 inserts.
struct TextEdit {
    std::string file;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::string new_text;
};

// Where a commit writes (designer-protocol-v1 section 10.1).
enum class PatchDestination : std::uint8_t {
    Inline, // the `style` attribute of the JSX element for the node
    Class,  // the rule for `.className` in the project CSS file named by `file`
    Token,  // the value of the existing token declaration in the token file
};

// A commit request, as the page sends it. `class_name` and `file` are the `class` destination's
// fields; `token` is the `token` destination's. The unused ones are empty.
struct PatchRequest {
    std::uint64_t automation_id = 0;
    PatchDestination destination = PatchDestination::Inline;
    std::string class_name;
    std::string file;
    std::string token;
    // The value the token declaration takes when `destination` is `Token`. The declarations below
    // are all `var(<token>)` - they name the binding, not the value - so the new value has to be
    // carried separately: it is what the override resolved to, which only the caller knows.
    std::string token_value;
    // The declarations the commit writes, in the order the override layer holds them. A commit
    // writes ALL of a node's overrides together (section 10.1), so this is the whole list, not one
    // property.
    std::vector<std::pair<std::string, std::string>> declarations;
};

// The spec's failure vocabulary, exactly (designer-protocol-v1 section 10.2). No patch is sent for
// any of them.
enum class PatchErrorCode : std::uint8_t {
    NoOverrides,      // the node has no overrides to commit
    NotTokenBound,    // a `token` commit whose override is not `var(<token>)`
    NoSuchNode,       // the NodeRef names no node in the running evaluation
    NoSuchClassFile,  // the `class` destination names a file the project does not have
    ComponentInvalid, // the component has no valid source span to edit
    StaleSource,      // the viewer's version of a file is older than one it was told about
};

// The spec's spelling of a failure reason, which is what the page and the host branch on.
//
// Defined here rather than in the `.cpp` because it is a fixed vocabulary, not logic: the
// serializer
// (`canvas_report.cpp`) is the other consumer and is compiled into targets that do not link the
// patch builder, so an out-of-line definition would make every such target depend on the whole
// builder for one `switch`. An enum-to-string table has no state to share, so there is nothing to
// gain by hiding it.
[[nodiscard]] inline std::string_view patch_error_name(PatchErrorCode code) noexcept {
    switch (code) {
    case PatchErrorCode::NoOverrides:
        return "no_overrides";
    case PatchErrorCode::NotTokenBound:
        return "not_token_bound";
    case PatchErrorCode::NoSuchNode:
        return "no_such_node";
    case PatchErrorCode::NoSuchClassFile:
        return "no_such_class_file";
    case PatchErrorCode::ComponentInvalid:
        return "component_invalid";
    case PatchErrorCode::StaleSource:
        return "stale_source";
    }
    return "no_such_node";
}

struct PatchError {
    PatchErrorCode code = PatchErrorCode::NoSuchNode;
    // A human-readable detail for the page's status line. Never part of the protocol's `reason`,
    // which is the fixed vocabulary above.
    std::string detail;
};

// A patch ready for the host: the edits and the version of every file they touch.
struct SourcePatch {
    // Non-overlapping, sorted by file and DESCENDING offset, so applying them in order never shifts
    // a later edit (designer-protocol-v1 section 10.2). The ordering is a property of this value,
    // not a convention a caller has to restore.
    std::vector<TextEdit> edits;
    // Each affected file path to the version the viewer used to compute the offsets. The host
    // compares these with its own copies and applies nothing if any differs (section 10.3).
    std::vector<std::pair<std::string, std::uint32_t>> base_versions;
};

// The answer to one commit: either a patch, or the spec's reason it has none. This is the shape the
// report serializer and the C ABI export take, because `std::expected` is not a type a report can
// branch on without the caller unwrapping it first.
struct SourcePatchResult {
    bool ok = false;
    // The viewer's handle for this patch, which `style.commit.result` carries so the following
    // `source.patch` can name it (section 10.2). Empty when `ok` is false.
    std::string patch_id;
    SourcePatch patch;
    // The spec's failure vocabulary, exactly. Meaningful only when `ok` is false.
    PatchErrorCode reason = PatchErrorCode::NoSuchNode;
    // A human-readable detail for the page's status line, never part of the protocol's `reason`.
    std::string detail;
};

// What the patch builder needs to know about the running evaluation. It is a value, not a session
// reference, so a test can build a patch from a fixture without a live tree.
struct PatchContext {
    // The project's files as the viewer staged them: the bytes the offsets are measured in. Owned
    // here rather than borrowed as a `ProjectInput` so a caller can build a context from a fixture
    // without keeping a second array alive; `project()` is the borrowed view the builder takes.
    std::vector<ProjectFile> sources;
    std::vector<ProjectFile> stylesheets;
    std::string entry;
    // False when the NodeRef names no node in the running evaluation, which is `no_such_node`.
    bool node_found = false;
    // The IR span of the node being committed, from the session's side table. Empty `file` means
    // the node has no span, which is `component_invalid`.
    std::string file;
    std::uint32_t span_offset = 0;
    std::uint32_t span_length = 0;
    // The node's `className` attribute value, when it has one. Empty means the element lists no
    // class, so a `class` commit must add the attribute as well as the rule.
    std::string class_name;
    // The theme asset (`assets.theme`), which is not part of the project's sources because the
    // preview never compiles it. `theme_id` is the active theme, the one whose token a commit
    // edits.
    std::string theme_file;
    std::string theme_text;
    std::string theme_id;
    std::uint32_t theme_version = 0;
    // The version the viewer holds for each project file. A file absent from this list is one the
    // viewer never staged, which is `stale_source` for a commit that would edit it.
    std::vector<std::pair<std::string, std::uint32_t>> versions;

    // The borrowed view of `sources`/`stylesheets`/`entry` the builder reads. The views point into
    // this context, so the context must outlive the call.
    [[nodiscard]] ProjectInput project() const {
        ProjectInput input;
        input.sources = sources;
        input.stylesheets = stylesheets;
        input.entry = entry;
        return input;
    }
};

// Builds the patch for `request` against `context`.
//
// The three destinations produce different edits, and each one is the minimal replacement the spec
// asks for:
//
//   * `inline` replaces the `style` attribute's bytes with the new attribute text, or inserts one
//     (`length == 0`) at the end of the opening tag when the element has none. The new text keeps
//     every declaration the attribute already had, so a commit never drops an inline value the
//     developer wrote;
//   * `class` replaces the declarations of the `.className` rule that the commit changes and
//   inserts
//     the ones it adds, or appends the whole rule to the file when it is absent;
//   * `token` replaces the value of the existing token declaration in the theme asset.
//
// Text outside the replaced ranges - formatting, comments, the `// Note: bind::<id>` comments and
// their handlers - is never part of an edit.
[[nodiscard]] std::expected<SourcePatch, PatchError>
build_source_patch(const PatchRequest& request, const PatchContext& context);

} // namespace tessera::compiler::codegen::cli
