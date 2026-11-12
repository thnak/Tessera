// SPDX-License-Identifier: MIT
//
// Snapshot half of EngineAutomationHost: it walks the live scene into the automation snapshot
// builder (docs/specs/automation-ids-v1.md section 5.2) and returns the canonical document. Kept
// separate from the action half so both translation units stay small.

#include "tessera/automation/engine_host.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/automation/snapshot.hpp"
#include "tessera/ui/automation/snapshot_json.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cmath>
#include <cstddef>
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

std::string_view accessible_name(const ui::Node& node) {
    for (const ui::Node::AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == "aria-label") {
            return attribute.value;
        }
    }
    return {};
}

std::string_view debug_kind(const ui::Node& node) {
    const std::string_view kind = node.element_kind();
    return kind.empty() ? std::string_view("Node") : kind;
}

// Structural path, built from parent to child in source order and capped so a deep tree cannot make
// one path (and so the snapshot) unbounded. Deterministic for a fixed tree.
std::string path_for(std::string_view parent, const ui::Node& node, std::size_t index) {
    std::string path(parent);
    path.push_back('/');
    path.append(debug_kind(node));
    path.push_back('[');
    path.append(std::to_string(index));
    path.push_back(']');
    if (path.size() > kMaxSnapshotPathBytes) {
        path.resize(kMaxSnapshotPathBytes);
    }
    return path;
}

std::expected<ui::automation::SnapshotSourceNode, harness::ProtocolError>
build_source_node(const ui::Node& node, std::string_view path, bool parent_visible,
                  std::size_t depth, std::size_t cap, std::size_t& count,
                  const NodeAnnotations& annotations) {
    if (depth > kMaxSnapshotDepth) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "snapshot tree exceeds the depth cap",
                                std::to_string(kMaxSnapshotDepth)));
    }
    if (++count > cap) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "snapshot tree exceeds the node cap", std::to_string(cap)));
    }

    ui::automation::SnapshotSourceNode source;
    source.ui_id = node.automation_id();
    source.component_id = 0; // the scene carries no component id; annotations supply it
    source.kind = std::string(debug_kind(node));
    source.path = std::string(path);
    if (const auto found = annotations.find(node.automation_id()); found != annotations.end()) {
        source.component_id = found->second.component_id;
        if (!found->second.path.empty()) {
            source.path = found->second.path.substr(0, kMaxSnapshotPathBytes);
        }
    }
    source.name = std::string(accessible_name(node));
    if (const auto key = node.item_key(); key.has_value()) {
        source.item_key = std::to_string(*key); // canonical decimal, matches resolve_target
    }
    const auto& paint = node.paint_data();
    if (paint.has_value() && paint->kind == ui::PaintKind::Text) {
        source.text = paint->text;
    }
    const auto bounds = node.world_bounds();
    if (std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
        std::isfinite(bounds.height)) {
        source.bounds =
            ui::automation::SnapshotBounds{bounds.x, bounds.y, bounds.width, bounds.height};
    }
    source.visible = parent_visible && node.visible();
    source.enabled = !ui::style::has_state(node.pseudo_state(), ui::style::PseudoState::Disabled);
    source.focused = ui::style::has_state(node.pseudo_state(), ui::style::PseudoState::Focus);
    source.selected = ui::style::has_state(node.pseudo_state(), ui::style::PseudoState::Checked);

    source.children.reserve(node.children().size());
    for (std::size_t index = 0; index < node.children().size(); ++index) {
        const ui::Node& child = *node.children()[index];
        auto built = build_source_node(child, path_for(path, child, index), source.visible,
                                       depth + 1, cap, count, annotations);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        source.children.push_back(std::move(*built));
    }
    return source;
}

} // namespace

std::expected<harness::JsonValue, harness::ProtocolError>
EngineAutomationHost::canonical_snapshot(bool include_hidden) {
    settle();

    std::size_t count = 0;
    auto source = build_source_node(root_, "root", true, 0, config_.max_snapshot_nodes, count,
                                     config_.annotations);
    if (!source.has_value()) {
        return std::unexpected(std::move(source.error()));
    }

    ui::automation::SnapshotOptions options;
    options.sequence = snapshot_sequence_++;
    options.include_hidden = include_hidden;
    const ui::automation::UiSnapshot snapshot = ui::automation::build_snapshot(*source, options);

    auto parsed = harness::parse_json(ui::automation::snapshot_to_json(snapshot));
    if (!parsed.has_value()) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "canonical snapshot document could not be decoded", "snapshot"));
    }
    stats_.emitted_nodes = snapshot.nodes.size();
    ++stats_.snapshots;
    record("automation.snapshot", diagnostics::TraceCategory::Frame);
    return std::move(*parsed);
}

} // namespace tessera::automation
