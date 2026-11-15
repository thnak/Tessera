// SPDX-License-Identifier: MIT

#include "scene_evaluate.hpp"
#include "scene_preview.hpp"
#include "scene_session.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

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

// The stylesheet the preview lays out with. The page has one buffer and no project, so there is no
// CSS to read: the sheet is the preview's own default rules for the intrinsic element kinds, which
// give the nodes a size instead of collapsing every box to zero. A custom property or class in the
// sample is simply unmatched, exactly as it would be in a project that ships no CSS.
std::string_view preview_stylesheet() {
    return "View { display: flex; flex-direction: column; }\n"
           "Text { display: block; height: 24px; font-size: 16px; }\n"
           "Button { width: 120px; height: 32px; }\n"
           "TextInput { width: 180px; height: 28px; }\n"
           "TextArea { width: 180px; height: 64px; }\n"
           "Image { width: 64px; height: 64px; }\n"
           "Spacer { height: 8px; }\n"
           "Scroll { width: 200px; height: 80px; }\n";
}

} // namespace

std::string scene_preview_document(std::string_view text, const ScenePreviewOptions& options,
                                   std::string_view component) {
    auto evaluated = evaluate_source(text, options.file, options.salt, component);
    if (!evaluated) {
        return empty_document(0);
    }

    const ui::style::CompiledStyleSheet sheet = ui::style::compile_stylesheet(preview_stylesheet());
    PreviewSession::Options session_options;
    session_options.width = options.width;
    session_options.height = options.height;
    session_options.dpi_scale = options.dpi_scale;
    PreviewSession session(evaluated->module, evaluated->selected(), sheet, session_options);
    if (!session.built()) {
        return empty_document(0);
    }

    // The canonical document, from the walk the native harness uses (issue #166): one
    // implementation of the artifact, so the parity gate compares the pipeline and not two
    // serializers.
    ui::automation::SceneSnapshotOptions snapshot_options;
    snapshot_options.snapshot.sequence = 0;
    auto snapshot = ui::automation::snapshot_scene(session.root(), snapshot_options);
    if (!snapshot.has_value()) {
        return empty_document(0);
    }
    return ui::automation::snapshot_to_json(*snapshot);
}

} // namespace tessera::compiler::codegen::cli
