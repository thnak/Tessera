// SPDX-License-Identifier: MIT
//
// Snapshot half of EngineAutomationHost: it asks the shared scene walk
// (`ui::automation::snapshot_scene`, src/ui/automation/scene_snapshot.cpp) for the canonical
// UiSnapshot of the live tree (docs/specs/automation-ids-v1.md section 5.2) and returns the
// document. The walk lives in the automation lane, not here, because the design-time preview's
// in-memory evaluate -> layout -> snapshot path must produce the same bytes (issue #166); this file
// only supplies the harness's manifest annotations and its sequence counter.

#include "tessera/automation/engine_host.hpp"
#include "tessera/automation/node_annotation.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/automation/snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::automation {
namespace {

namespace harness = tessera::harness;
namespace ui = tessera::ui;

harness::ProtocolError error(harness::ProtocolErrorCode code, std::string message,
                             std::string detail = {}) {
    return harness::ProtocolError{code, std::move(message), std::move(detail), std::nullopt};
}

std::unexpected<harness::ProtocolError> unexpected(harness::ProtocolError value) {
    return std::unexpected(std::move(value));
}

// The manifest annotation as the walk's callback (`SceneAnnotationFn`). `NodeAnnotations` is keyed
// by automation id, exactly what the walk passes in.
struct AnnotationContext {
    const NodeAnnotations* annotations = nullptr;
};

bool annotate(void* raw, std::uint64_t automation_id, ui::automation::SceneAnnotation& out) {
    const auto* context = static_cast<const AnnotationContext*>(raw);
    if (context == nullptr || context->annotations == nullptr) {
        return false;
    }
    const auto found = context->annotations->find(automation_id);
    if (found == context->annotations->end()) {
        return false;
    }
    out.path = found->second.path;
    out.component_id = found->second.component_id;
    return true;
}

std::string_view error_name(ui::automation::SceneSnapshotErrorCode code) {
    switch (code) {
    case ui::automation::SceneSnapshotErrorCode::TooDeep:
        return "snapshot tree exceeds the depth cap";
    case ui::automation::SceneSnapshotErrorCode::TooManyNodes:
        return "snapshot tree exceeds the node cap";
    }
    return "snapshot tree exceeds a cap";
}

} // namespace

std::expected<harness::JsonValue, harness::ProtocolError>
EngineAutomationHost::canonical_snapshot(bool include_hidden) {
    settle();
    // A settle may have run layout, which assigns flow bounds to realized item nodes; restore the
    // window placement before reading bounds. This also realizes the initial window on the first
    // snapshot after layout.
    refresh_virtual_collections();

    AnnotationContext annotations{&config_.annotations};
    ui::automation::SceneSnapshotOptions options;
    options.snapshot.sequence = snapshot_sequence_++;
    options.snapshot.include_hidden = include_hidden;
    options.max_nodes = config_.max_snapshot_nodes;
    options.max_depth = kMaxSnapshotDepth;
    options.annotate = &annotate;
    options.annotation_context = &annotations;

    auto snapshot = ui::automation::snapshot_scene(root_, options);
    if (!snapshot.has_value()) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                std::string(error_name(snapshot.error().code)),
                                std::to_string(snapshot.error().limit)));
    }

    auto parsed = harness::parse_json(ui::automation::snapshot_to_json(*snapshot));
    if (!parsed.has_value()) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "canonical snapshot document could not be decoded", "snapshot"));
    }
    stats_.emitted_nodes = snapshot->nodes.size();
    ++stats_.snapshots;
    record("automation.snapshot", diagnostics::TraceCategory::Frame);
    return std::move(*parsed);
}

} // namespace tessera::automation
