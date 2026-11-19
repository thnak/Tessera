// SPDX-License-Identifier: MIT

#include "query_tools.hpp"
#include "scene_canvas_inspect.hpp"
#include "scene_prepare.hpp"
#include "tessera/harness/json_write.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <string>
#include <utility>

namespace tessera::mcp {
namespace {

namespace cli = tessera::compiler::codegen::cli;

JsonValue text(std::string_view value) {
    return JsonValue::string(std::string(value));
}

JsonValue edge_array(const std::array<float, 4>& edges) {
    JsonValue values = JsonValue::array();
    for (const float edge : edges) {
        values.push_back(JsonValue::number(edge));
    }
    return values;
}

// The border box in logical viewport pixels, the space `snapshot`'s bounds are in.
JsonValue box_json(float x, float y, float width, float height) {
    return JsonValue::object({{"x", JsonValue::number(x)},
                              {"y", JsonValue::number(y)},
                              {"width", JsonValue::number(width)},
                              {"height", JsonValue::number(height)}});
}

ToolResult encode(const JsonValue& document, std::string_view tool) {
    auto encoded = tessera::harness::to_json(document);
    if (!encoded) {
        return ToolResult{true, std::string(tool) + ": the result could not be encoded (code " +
                                    std::to_string(static_cast<int>(encoded.error().code)) +
                                    ", offset " + std::to_string(encoded.error().offset) + ")"};
    }
    return ToolResult{false, std::move(*encoded)};
}

JsonValue style_json(const cli::StyleInfoResult& info) {
    if (!info.found) {
        // The spec's whole answer for a missing node: no other field (designer-protocol-v1 9.1), so
        // a client cannot read a stale `computed` off a miss.
        return JsonValue::object({{"styleVersion", JsonValue::integer(1)},
                                  {"ok", JsonValue::boolean(false)},
                                  {"reason", text("no_such_node")}});
    }
    JsonValue computed = JsonValue::object();
    for (const auto& [property, value] : info.computed) {
        computed.set(property, text(value));
    }
    JsonValue matched = JsonValue::array();
    for (const cli::StyleMatchedRule& rule : info.matched) {
        JsonValue declarations = JsonValue::array();
        for (const cli::StyleDeclarationInfo& declaration : rule.declarations) {
            declarations.push_back(
                JsonValue::object({{"property", text(declaration.property)},
                                   {"value", text(declaration.value)},
                                   {"important", JsonValue::boolean(declaration.important)},
                                   {"overridden", JsonValue::boolean(declaration.overridden)}}));
        }
        JsonValue specificity = JsonValue::array();
        for (const std::uint32_t part : rule.specificity) {
            specificity.push_back(JsonValue::integer(part));
        }
        // The rule's file is the project-relative name it was compiled under, never a host path.
        matched.push_back(JsonValue::object(
            {{"origin", text(rule.origin)},
             {"selector", text(rule.selector)},
             {"specificity", std::move(specificity)},
             {"span", JsonValue::object({{"file", text(rule.file)},
                                         {"offset", JsonValue::integer(rule.offset)},
                                         {"length", JsonValue::integer(rule.length)}})},
             {"declarations", std::move(declarations)}}));
    }
    JsonValue box = box_json(info.box.x, info.box.y, info.box.width, info.box.height);
    box.set("margin", edge_array(info.box.margin));
    box.set("border", edge_array(info.box.border));
    box.set("padding", edge_array(info.box.padding));
    return JsonValue::object({{"styleVersion", JsonValue::integer(1)},
                              {"ok", JsonValue::boolean(true)},
                              {"node", text(std::to_string(info.automation_id))},
                              {"computed", std::move(computed)},
                              {"matched", std::move(matched)},
                              {"box", std::move(box)}});
}

// A scene that did not evaluate has no tree to query. The full compiler diagnostics are one
// `diagnostics` call away, so this names the stage's own text and points there.
ToolResult not_evaluated(std::string_view tool, const std::string& error) {
    return ToolResult{true, std::string(tool) + ": the source did not evaluate (" + error +
                                "); call `diagnostics` for the compiler's report"};
}

} // namespace

ToolResult run_query_style_tool(const ViewportState& viewport, std::string_view source,
                                std::string_view file, std::uint64_t ui_id) {
    const cli::ScenePreviewOptions options = scene_preview_options(viewport, file);
    const cli::PreparedScene scene = cli::prepare_scene(source, options);
    if (!scene.error.empty() || scene.root == nullptr || !scene.sheet.has_value()) {
        return not_evaluated("query_style", scene.error);
    }
    return encode(style_json(cli::style_info_for(*scene.root, *scene.sheet, ui_id)), "query_style");
}

ToolResult run_pick_tool(const ViewportState& viewport, std::string_view source,
                         std::string_view file, float x, float y) {
    const cli::ScenePreviewOptions options = scene_preview_options(viewport, file);
    cli::PreparedScene scene = cli::prepare_scene(source, options);
    if (!scene.error.empty() || scene.root == nullptr) {
        return not_evaluated("pick", scene.error);
    }
    // The router's own hit test, so the node named here is the node a press at the same point
    // would reach (`pointer-events`, scroll clipping and z-order included).
    const tessera::ui::Node* hit = scene.root->hit_test_deep(x, y);
    JsonValue document = JsonValue::object({{"pickVersion", JsonValue::integer(1)}});
    if (hit == nullptr) {
        document.set("found", JsonValue::boolean(false));
        document.set("automationId", text("0"));
        document.set("elementKind", text(""));
        document.set("box", box_json(0, 0, 0, 0));
        return encode(document, "pick");
    }
    const auto bounds = hit->world_bounds();
    document.set("found", JsonValue::boolean(true));
    document.set("automationId", text(std::to_string(hit->automation_id())));
    document.set("elementKind", text(hit->element_kind()));
    document.set("box", box_json(bounds.x, bounds.y, bounds.width, bounds.height));
    return encode(document, "pick");
}

} // namespace tessera::mcp
