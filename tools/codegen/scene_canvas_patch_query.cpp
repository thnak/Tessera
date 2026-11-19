// SPDX-License-Identifier: MIT

// The session's `source.patch` query (issue #171, designer-protocol-v1 section 10.2): it gathers
// the facts the patch builder needs from the running evaluation and hands them over.
//
// **Why this is a separate unit from the builder.** The builder is a pure function of a
// `PatchContext`; this is the part that knows where a session keeps those facts - the side table of
// IR spans, the staged project bytes, the node's class name. Keeping them apart is what lets the
// builder be tested from a fixture with no live tree, which is how the byte-range assertions in
// `scene_canvas_patch_tests.cpp` can be exact.

#include "scene_canvas_session.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

// The viewer's handle for a patch: the node and the destination, so two commits of the same node to
// the same destination in one session get the same id and a host can tell a retry from a new commit
// (section 10.2's "no new `source.patch` for the same node while one is open").
std::string patch_id_for(const PatchRequest& request) {
    std::string id = "patch:";
    id += std::to_string(request.automation_id);
    id += ':';
    switch (request.destination) {
    case PatchDestination::Inline:
        id += "inline";
        break;
    case PatchDestination::Class:
        id += "class:";
        id += request.class_name;
        break;
    case PatchDestination::Token:
        id += "token:";
        id += request.token;
        break;
    }
    return id;
}

} // namespace

SourcePatchResult CanvasSceneSession::patch(const PatchRequest& request) const {
    SourcePatchResult result;
    if (!started()) {
        result.reason = PatchErrorCode::NoSuchNode;
        result.detail = "no evaluation is running";
        return result;
    }

    PatchContext context;
    // The staged project, as the session holds it: the bytes the offsets are measured in. The
    // session's copy is the one the running evaluation was built from, so a patch computed here
    // cannot describe a file the tree on screen was not built from.
    for (const auto& [path, text] : project_.sources) {
        context.sources.push_back(ProjectFile{path, text});
    }
    for (const auto& [path, text] : project_.stylesheets) {
        context.stylesheets.push_back(ProjectFile{path, text});
    }
    context.entry = project_.entry;
    // Every staged file is at the version the viewer holds. The page's `source.update` is what
    // moves a version, and this slice does not receive one, so the viewer's own count is the base.
    for (const auto& [path, text] : project_.sources) {
        (void)text;
        context.versions.emplace_back(path, 1);
    }
    for (const auto& [path, text] : project_.stylesheets) {
        (void)text;
        context.versions.emplace_back(path, 1);
    }

    // The node: its span from the side table, and its class name from the live tree. A node the
    // automation id does not name is `no_such_node`, which the builder answers from `node_found`.
    const ir::SourceSpan* span = node_span(request.automation_id);
    if (span != nullptr) {
        context.node_found = true;
        context.file = span->file;
        context.span_offset = span->offset;
        context.span_length = span->length;
    }
    if (const ui::Node* node = session_->find_by_automation_id(request.automation_id)) {
        context.class_name = node->class_name();
    }

    const std::expected<SourcePatch, PatchError> built = build_source_patch(request, context);
    if (!built) {
        result.reason = built.error().code;
        result.detail = built.error().detail;
        return result;
    }
    result.ok = true;
    result.patch_id = patch_id_for(request);
    result.patch = *built;
    return result;
}

} // namespace tessera::compiler::codegen::cli
