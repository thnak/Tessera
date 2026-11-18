// SPDX-License-Identifier: MIT

#include "scene_prepare.hpp"
#include "scene_preview.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// The document for a run that produced no tree. It is a *valid* document of the same version with
// an empty `nodes` array, so a consumer parses one shape, and the parity gate's minimum-node-count
// guard is what stops "both sides empty" from passing.
std::string empty_document(std::uint64_t sequence) {
    ui::automation::UiSnapshot snapshot;
    snapshot.sequence = sequence;
    return ui::automation::snapshot_to_json(snapshot);
}

std::string document_of(PreparedScene scene) {
    // The stage sequence lives in `scene_prepare.cpp` so this document and the P3 painter
    // (`scene_frame.cpp`) cannot disagree about what the preview built: one pipeline, three
    // consumers. A failed stage is an empty document here, not an error beside it.
    //
    // It is also the stage order the P4 interaction session (`scene_session.cpp`) and the P5 sized
    // export reach: `prepare_scene()` runs the parse -> analyse -> lower -> identity -> evaluate ->
    // build -> style -> layout sequence, so a snapshot and a scripted click see one tree.
    if (!scene.error.empty() || scene.root == nullptr) {
        return empty_document(0);
    }

    // The canonical document, from the walk the native harness uses (issue #166): one
    // implementation of the artifact, so the parity gate compares the pipeline and not two
    // serializers.
    ui::automation::SceneSnapshotOptions snapshot_options;
    snapshot_options.snapshot.sequence = 0;
    auto snapshot = ui::automation::snapshot_scene(*scene.root, snapshot_options);
    if (!snapshot.has_value()) {
        return empty_document(0);
    }
    return ui::automation::snapshot_to_json(*snapshot);
}

} // namespace

std::string scene_preview_document(std::string_view text, const ScenePreviewOptions& options,
                                   std::string_view component) {
    // One file is a project of one (see `prepare_scene()`).
    return document_of(prepare_scene(text, options, component));
}

std::string scene_preview_document(const ProjectInput& project, const ScenePreviewOptions& options,
                                   std::string_view component) {
    return document_of(prepare_scene(project, options, component));
}

} // namespace tessera::compiler::codegen::cli
