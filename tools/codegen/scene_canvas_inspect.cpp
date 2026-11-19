// SPDX-License-Identifier: MIT

#include "scene_canvas_inspect.hpp"
#include "scene_canvas_session.hpp"

#include <cstdint>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace ui = tessera::ui;

StyleInfoResult CanvasSceneSession::inspect(std::uint64_t automation_id) {
    if (!started()) {
        return StyleInfoResult{};
    }
    StyleInfoResult result = style_info_for(session_->root(), sheet(), automation_id);
    if (result.found) {
        // The override layer is the session's, not the cascade's: `computed` above already
        // reflects it (an override is applied to the node), and this lists what was set.
        result.overrides = overrides_of(automation_id);
    }
    return result;
}

const ir::SourceSpan* CanvasSceneSession::node_span(std::uint64_t automation_id) const {
    if (!started() || automation_id == 0) {
        return nullptr;
    }
    const ui::Node* node = find_node_by_automation_id(session_->root(), automation_id);
    if (node == nullptr) {
        return nullptr;
    }
    // The side table is keyed by the SCENE node id, which is what the build walk assigned; the
    // automation id is a different number (it comes from the IR's `automationId`/`uiId`), so the
    // lookup goes through the node the automation id named.
    for (const SceneBuildResult::SceneNodeSource& entry : session_->node_sources()) {
        if (entry.node_id == node->id()) {
            return &entry.span;
        }
    }
    return nullptr;
}

} // namespace tessera::compiler::codegen::cli