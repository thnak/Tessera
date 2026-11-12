// SPDX-License-Identifier: MIT

// `Path` -> `PaintKind::Vector` (docs/design/compiler.md section 11). `PaintVectorStyle::asset` is
// an embedded-asset name, while the intrinsic carries inline `d` + `viewBox`. Codegen therefore
// synthesizes one SVG document per `Path` node from the literal props, embeds the exact bytes as a
// generated asset bundle, and names that asset in the emitted `PaintData`. The existing
// asset -> SVG decode -> path rasterizer chain paints it: no new paint payload and no new RHI path.
//
// Determinism: every byte is a function of the IR (fixed attribute order, shortest round-trip
// numbers, no timestamps, no map iteration order, no locale). Regenerating the same source is
// byte-identical, and the asset name is derived from the stable identity (`uiId`, the salt-derived
// node identity) so inserting an unrelated node does not rename it.

#include "emit_vector.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

constexpr std::size_t kBytesPerLine = 12;
constexpr char kHexDigits[] = "0123456789abcdef";
// The normalized-asset-name charset (include/tessera/ui/assets/asset_bundle.hpp).
constexpr std::string_view kAssetMime = "image/svg+xml";

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

std::string byte_text(std::uint8_t value) {
    std::string text("0x");
    text += kHexDigits[(value >> 4U) & 0xFU];
    text += kHexDigits[value & 0xFU];
    return text;
}

const ir::Expr* prop_of(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

std::optional<std::string_view> literal_text_of(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

// The four characters that would end an attribute value or start markup. `d` is path data (no
// markup characters in well-formed input) and the colours are CSS tokens, so escaping keeps a
// malformed literal from producing a document the decoder must reject.
std::string xml_attribute(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// Shortest round-trip text, the same rule the expression emitter uses for doubles. `to_chars` is
// locale-independent, so a `strokeWidth` never depends on the host locale.
std::string number_text(double value) {
    char buffer[64];
    const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof buffer, value);
    return std::string(buffer, result.ptr);
}

bool is_separator(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',';
}

// The spec's `viewBox` rule (jsx-subset-v1 section 11.1): four numbers, width and height greater
// than zero. A malformed value is rejected here instead of producing a document the runtime
// decoder silently drops.
bool valid_view_box(std::string_view text) {
    std::array<double, 4> values{};
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && is_separator(text[index])) {
            ++index;
        }
        if (index >= text.size()) {
            break;
        }
        if (count == values.size()) {
            return false;
        }
        const char* begin = text.data() + index;
        const char* end = text.data() + text.size();
        double value = 0.0;
        const std::from_chars_result parsed = std::from_chars(begin, end, value);
        if (parsed.ec != std::errc{}) {
            return false;
        }
        values[count++] = value;
        index = static_cast<std::size_t>(parsed.ptr - text.data());
        if (index < text.size() && !is_separator(text[index])) {
            return false;
        }
    }
    if (count != values.size()) {
        return false;
    }
    return values[2] > 0.0 && values[3] > 0.0;
}

bool safe_identity(std::string_view text) {
    if (text.empty()) {
        return false;
    }
    for (const char c : text) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

// The embedded-asset name of one `Path` node: `vector/<Component>/<identity>.svg`. `identity` is
// the node's stable `uiId` (the salt-derived identity from src/compiler/ids) so an unrelated node
// insertion does not rename the asset; without an identity salt the pre-order ordinal is the
// deterministic placeholder, exactly like the generated `NodeId`.
std::string asset_name_of(std::string_view component_name, const ir::Node& node, unsigned ordinal) {
    const std::string identity =
        safe_identity(node.ui_id) ? node.ui_id : "node" + std::to_string(ordinal);
    return "vector/" + std::string(component_name) + "/" + identity + ".svg";
}

// A `var(--name)` literal cannot be resolved into a standalone SVG document (there is no
// stylesheet in the document), so it is rejected instead of producing an undecodable paint.
bool reject_var_reference(std::string_view prop_name, std::string_view value, std::string& error) {
    if (value.find("var(") == std::string_view::npos) {
        return true;
    }
    return fail(error, "`" + std::string(prop_name) + "` is `" + std::string(value) +
                           "`; a synthesized `Path` asset has no stylesheet to resolve `var()` "
                           "against, so use a literal color");
}

bool build_svg(std::string_view view_box, std::string_view d, std::string_view fill,
               std::string_view stroke, std::string_view stroke_width, std::string& svg) {
    // Fixed attribute order and no optional attributes: the bytes are a pure function of the IR.
    svg.clear();
    svg += "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"";
    svg += xml_attribute(view_box);
    svg += "\"><path d=\"";
    svg += xml_attribute(d);
    svg += "\" fill=\"";
    svg += xml_attribute(fill);
    svg += "\" stroke=\"";
    svg += xml_attribute(stroke);
    svg += "\" stroke-width=\"";
    svg += stroke_width; // already formatted number text, never user text
    svg += "\"/></svg>";
    return true;
}

} // namespace

bool build_vector_payload(std::string_view component_name, const ir::Node& node, unsigned ordinal,
                          VectorPayload& payload, std::string& error) {
    const std::optional<std::string_view> d = literal_text_of(prop_of(node, "d"));
    if (!d) {
        return fail(error, "`Path` node `" + node.id +
                               "` has no literal `d`; only a string literal is supported");
    }
    if (d->empty()) {
        return fail(error, "`Path` node `" + node.id + "` has an empty `d`");
    }
    const std::optional<std::string_view> view_box = literal_text_of(prop_of(node, "viewBox"));
    if (!view_box) {
        return fail(error, "`Path` node `" + node.id +
                               "` has no literal `viewBox`; only a string literal is supported");
    }
    if (!valid_view_box(*view_box)) {
        return fail(error, "`Path` node `" + node.id + "` has `viewBox` `" +
                               std::string(*view_box) +
                               "`; expected four numbers with width and height greater than zero");
    }

    std::string fill = "none";
    if (const ir::Expr* value = prop_of(node, "fill")) {
        const std::optional<std::string_view> text = literal_text_of(value);
        if (!text) {
            return fail(error, "`Path` node `" + node.id +
                                   "` has a non-literal `fill`; only a literal is supported");
        }
        if (!reject_var_reference("fill", *text, error)) {
            return false;
        }
        fill = std::string(*text);
    }
    std::string stroke = "none";
    if (const ir::Expr* value = prop_of(node, "stroke")) {
        const std::optional<std::string_view> text = literal_text_of(value);
        if (!text) {
            return fail(error, "`Path` node `" + node.id +
                                   "` has a non-literal `stroke`; only a literal is supported");
        }
        if (!reject_var_reference("stroke", *text, error)) {
            return false;
        }
        stroke = std::string(*text);
    }
    std::string stroke_width = "1";
    if (const ir::Expr* value = prop_of(node, "strokeWidth")) {
        if (value->kind != ir::ExprKind::Literal) {
            return fail(error, "`Path` node `" + node.id +
                                   "` has a non-literal `strokeWidth`; only a number literal is "
                                   "supported");
        }
        if (const auto* whole = std::get_if<std::int64_t>(&value->literal)) {
            stroke_width = std::to_string(*whole);
        } else if (const auto* number = std::get_if<double>(&value->literal)) {
            stroke_width = number_text(*number);
        } else {
            return fail(error, "`Path` node `" + node.id +
                                   "` has a `strokeWidth` that is not a number literal");
        }
    }

    payload = VectorPayload{};
    payload.asset_name = asset_name_of(component_name, node, ordinal);
    build_svg(*view_box, *d, fill, stroke, stroke_width, payload.svg);
    if (const ir::Expr* label = prop_of(node, "label")) {
        const std::optional<std::string_view> text = literal_text_of(label);
        if (!text) {
            return fail(error, "`Path` node `" + node.id +
                                   "` has a non-literal `label`; only a string literal is "
                                   "supported as the accessible name");
        }
        payload.label = std::string(*text);
        payload.has_label = !payload.label.empty();
    }
    return true;
}

namespace {

// Pre-order walk matching emit_node's ordinal numbering: every node, then the item template of a
// virtual collection. `in_template` only affects nothing here (a `Path` in a list template still
// has one static document shared by every item), so the walk is a plain pre-order.
bool collect_walk(const ir::Component& component, const ir::Node& node, unsigned& ordinal,
                  std::vector<VectorAsset>& assets, std::string& error) {
    const unsigned self = ++ordinal;
    if (node.kind == ir::NodeKind::Path) {
        VectorPayload payload;
        if (!build_vector_payload(component.name, node, self, payload, error)) {
            return false;
        }
        assets.push_back(VectorAsset{std::move(payload.asset_name), std::move(payload.svg)});
    }
    for (const ir::Node& child : node.children) {
        if (!collect_walk(component, child, ordinal, assets, error)) {
            return false;
        }
    }
    if (node.item_template &&
        !collect_walk(component, *node.item_template, ordinal, assets, error)) {
        return false;
    }
    return true;
}

} // namespace

bool collect_vector_assets(const ir::IrModule& module, std::vector<VectorAsset>& assets,
                           std::string& error) {
    std::vector<const ir::Component*> components;
    components.reserve(module.components.size());
    for (const ir::Component& component : module.components) {
        components.push_back(&component);
    }
    std::sort(components.begin(), components.end(),
              [](const ir::Component* left, const ir::Component* right) {
                  return left->name < right->name;
              });
    for (const ir::Component* component : components) {
        unsigned ordinal = 0;
        if (!collect_walk(*component, component->root, ordinal, assets, error)) {
            return false;
        }
    }
    // The bundle's lookup invariant is a table sorted by normalized name, so this order is part of
    // the emitted bytes. A duplicate name would mean two documents claim one asset; reject it
    // instead of silently dropping one.
    std::sort(assets.begin(), assets.end(), [](const VectorAsset& left, const VectorAsset& right) {
        return left.name < right.name;
    });
    for (std::size_t i = 1; i < assets.size(); ++i) {
        if (assets[i - 1].name == assets[i].name) {
            return fail(error, "two `Path` nodes synthesize the same asset name `" +
                                   assets[i].name + "`; the node identity is not unique");
        }
    }
    return true;
}

std::string emit_vector_assets_header(std::string_view module_name,
                                      std::span<const VectorAsset> assets) {
    CppWriter writer;
    writer.line("// SPDX-License-Identifier: MIT");
    writer.line("// Generated by tessera_codegen. Do not edit.");
    writer.line("// Synthesized vector assets (docs/design/compiler.md section 11): one embedded");
    writer.line("// SVG per `Path` node, as the AssetBundle a sink's VectorPaintConfig takes.");
    writer.line("#pragma once");
    writer.line("#include \"tessera/ui/assets/asset_bundle.hpp\"");
    writer.line("#include <cstddef>");
    writer.line("#include <span>");
    writer.open("namespace " + std::string(module_name));
    writer.open("namespace gen");
    writer.open("namespace vector_assets");

    std::size_t total_bytes = 0;
    for (const VectorAsset& asset : assets) {
        total_bytes += asset.svg.size();
    }
    writer.line("inline constexpr std::size_t kAssetCount = " + std::to_string(assets.size()) +
                ";");
    writer.line("inline constexpr std::size_t kAssetBytes = " + std::to_string(total_bytes) + ";");
    writer.line("");
    writer.line("inline constexpr unsigned char kAssetData[] = {");
    ++writer.indent;
    std::string line_text;
    std::size_t written = 0;
    for (const VectorAsset& asset : assets) {
        for (const char c : asset.svg) {
            line_text += byte_text(static_cast<std::uint8_t>(c));
            line_text += ", ";
            ++written;
            if (written % kBytesPerLine == 0) {
                line_text.pop_back(); // keep the trailing comma, drop the space
                writer.line(line_text);
                line_text.clear();
            }
        }
    }
    if (!line_text.empty()) {
        line_text.pop_back();
        writer.line(line_text);
    }
    if (written == 0) {
        writer.line("0x00,"); // a C++ array is never zero-sized; kAssetBytes stays 0
    }
    --writer.indent;
    writer.line("};");
    writer.line("");
    // One entry per asset, offsets into the contiguous data blob above (the AssetBundle contract).
    writer.line("inline constexpr ::tessera::ui::assets::AssetEntry kAssetEntries[] = {");
    ++writer.indent;
    std::uint32_t offset = 0;
    for (const VectorAsset& asset : assets) {
        const auto size = static_cast<std::uint32_t>(asset.svg.size());
        writer.line("{\"" + asset.name + "\", " + std::to_string(offset) + ", " +
                    std::to_string(size) + ", \"" + std::string(kAssetMime) + "\"},");
        offset += size;
    }
    --writer.indent;
    writer.line("};");
    writer.line("");
    writer.line("[[nodiscard]] inline ::tessera::ui::assets::AssetBundle bundle() noexcept {");
    ++writer.indent;
    writer.line("return ::tessera::ui::assets::AssetBundle(");
    ++writer.indent;
    writer.line("std::span<const ::tessera::ui::assets::AssetEntry>(kAssetEntries, kAssetCount),");
    writer.line("std::as_bytes(std::span<const unsigned char>(kAssetData, kAssetBytes)));");
    --writer.indent;
    --writer.indent;
    writer.line("}");
    writer.close("");
    writer.close("");
    writer.close(" // namespace " + std::string(module_name));
    return writer.text;
}

bool emit_vector_paint(CppWriter& writer, const ir::Node& node, std::string_view component_name,
                       unsigned ordinal, std::string_view variable, std::string& error) {
    VectorPayload payload;
    if (!build_vector_payload(component_name, node, ordinal, payload, error)) {
        return false;
    }
    const std::string paint = "paint_" + std::string(variable);
    writer.line("tessera::ui::PaintData " + paint + ";");
    writer.line(paint + ".kind = tessera::ui::PaintKind::Vector;");
    writer.line(paint + ".vector.asset = \"" + payload.asset_name + "\";");
    writer.line(std::string(variable) + "->set_paint_data(std::move(" + paint + "));");
    return true;
}

bool emit_vector_label(CppWriter& writer, const ir::Node& node, std::string_view variable,
                       std::string& error) {
    const ir::Expr* label = prop_of(node, "label");
    if (label == nullptr) {
        return true;
    }
    if (prop_of(node, "aria-label") != nullptr) {
        return true; // an explicit aria-label already wins the name precedence
    }
    const std::optional<std::string_view> text = literal_text_of(label);
    if (!text) {
        return fail(error, "`Path` node `" + node.id +
                               "` has a non-literal `label`; only a string literal is supported "
                               "as the accessible name");
    }
    if (!text->empty()) {
        writer.line(std::string(variable) + "->set_aria(\"aria-label\", " +
                    literal_text(ir::LiteralValue{std::string(*text)}) + ");");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
