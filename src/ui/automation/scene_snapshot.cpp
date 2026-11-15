// SPDX-License-Identifier: MIT
//
// The scene -> snapshot walk (docs/specs/automation-ids-v1.md section 5.2), extracted from
// src/harness/engine_host_snapshot.cpp so the headless harness and the design-time preview's
// in-memory evaluate -> layout -> snapshot path (tools/codegen/scene_preview.cpp, issue #166) share
// one implementation of the artifact the parity gate compares.

#include "tessera/ui/automation/scene_snapshot.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <cmath>
#include <cstddef>
#include <exception>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::ui::automation {
namespace {

namespace style = tessera::ui::style;

std::string_view accessible_name(const Node& node) {
    for (const Node::AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == "aria-label") {
            return attribute.value;
        }
    }
    return {};
}

std::string_view debug_kind(const Node& node) {
    const std::string_view kind = node.element_kind();
    return kind.empty() ? std::string_view("Node") : kind;
}

// Structural path, built from parent to child in source order and capped so a deep tree cannot make
// one path (and so the snapshot) unbounded. Deterministic for a fixed tree.
std::string path_for(std::string_view parent, const Node& node, std::size_t index) {
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

std::unexpected<SceneSnapshotError> fail(SceneSnapshotErrorCode code, std::size_t limit,
                                         std::size_t actual) {
    return std::unexpected(SceneSnapshotError{code, limit, actual});
}

// Builds one source node and its subtree. `count` is the running node count, shared across the
// whole walk so the cap is a total, not a per-branch limit.
std::expected<SnapshotSourceNode, SceneSnapshotError>
build_source_node(const Node& node, std::string_view path, bool parent_visible, std::size_t depth,
                  const SceneSnapshotOptions& options, std::size_t& count) {
    if (depth > options.max_depth) {
        return fail(SceneSnapshotErrorCode::TooDeep, options.max_depth, depth);
    }
    if (++count > options.max_nodes) {
        return fail(SceneSnapshotErrorCode::TooManyNodes, options.max_nodes, count);
    }

    SnapshotSourceNode source;
    source.ui_id = node.automation_id();
    source.component_id = 0; // the scene carries no component id; an annotation supplies it
    source.kind = std::string(debug_kind(node));
    source.path = std::string(path);
    if (options.annotate != nullptr) {
        SceneAnnotation annotation;
        if (options.annotate(options.annotation_context, node.automation_id(), annotation)) {
            source.component_id = annotation.component_id;
            if (!annotation.path.empty()) {
                source.path = std::string(annotation.path.substr(0, kMaxSnapshotPathBytes));
            }
        }
    }
    source.name = std::string(accessible_name(node));
    if (const auto key = node.item_key(); key.has_value()) {
        source.item_key = std::to_string(*key); // canonical decimal, matches resolve_target
    }
    const auto& paint = node.paint_data();
    if (paint.has_value() && paint->kind == PaintKind::Text) {
        source.text = paint->text;
    }
    const auto bounds = node.world_bounds();
    if (std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
        std::isfinite(bounds.height)) {
        source.bounds = SnapshotBounds{bounds.x, bounds.y, bounds.width, bounds.height};
    }
    source.visible = parent_visible && node.rendered();
    source.enabled = !style::has_state(node.pseudo_state(), style::PseudoState::Disabled);
    source.focused = style::has_state(node.pseudo_state(), style::PseudoState::Focus);
    source.selected = style::has_state(node.pseudo_state(), style::PseudoState::Checked);

    source.children.reserve(node.children().size());
    for (std::size_t index = 0; index < node.children().size(); ++index) {
        const Node& child = *node.children()[index];
        auto built = build_source_node(child, path_for(path, child, index), source.visible,
                                       depth + 1, options, count);
        if (!built.has_value()) {
            return std::unexpected(std::move(built.error()));
        }
        source.children.push_back(std::move(*built));
    }
    return source;
}

} // namespace

std::expected<UiSnapshot, SceneSnapshotError> snapshot_scene(const Node& root,
                                                             const SceneSnapshotOptions& options) {
    // The bounds are caller input, and a zero cap would reject every tree including a one-node one;
    // treat it as the documented default instead of failing a run for a missing field.
    SceneSnapshotOptions effective = options;
    if (effective.max_nodes == 0) {
        effective.max_nodes = kMaxSnapshotNodes;
    }
    if (effective.max_depth == 0) {
        effective.max_depth = kMaxSnapshotDepth;
    }

    std::size_t count = 0;
    auto source = build_source_node(root, "root", true, 0, effective, count);
    if (!source.has_value()) {
        return std::unexpected(std::move(source.error()));
    }
    // build_snapshot() copies the source tree into preorder entries and drops hidden subtrees, and
    // is the one place that defines the emitted order (section 5.2). Its recursion is bounded by
    // the depth cap checked above.
    try {
        return build_snapshot(*source, effective.snapshot);
    } catch (const std::exception&) {
        // A pathological tree (an allocation failure while copying) is reported as a cap failure
        // rather than propagating: this walk is called from a C ABI on the Wasm side, where an
        // exception must never cross the boundary.
        return fail(SceneSnapshotErrorCode::TooManyNodes, effective.max_nodes, count);
    }
}

} // namespace tessera::ui::automation
