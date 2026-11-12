// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// `Path` -> `PaintKind::Vector` (docs/design/compiler.md section 11). `PaintVectorStyle::asset` is
// an embedded-asset name while the intrinsic carries inline `d`/`viewBox`, so codegen synthesizes
// one SVG document per `Path` node, embeds it as an asset, and names that asset in the payload.
// The existing asset -> SVG decode -> path rasterizer chain then paints it; no new rendering API.
namespace tessera::compiler::codegen::internal {

// One synthesized SVG document and the embedded-asset name it is registered under.
struct VectorAsset {
    std::string name; // normalized asset name, sorted by the emitted table
    std::string svg;  // the exact bytes embedded (deterministic)
};

// The resolved payload of one `Path` node: the asset name the generated code names, the SVG bytes
// the module bundle embeds, and the accessible name (`label`) when the node has one.
struct VectorPayload {
    std::string asset_name;
    std::string svg;
    std::string label;
    bool has_label = false;
};

// Resolves one `Path` node's payload. `ordinal` is the node's pre-order index, used only when the
// node has no stable `uiId` (a module generated without an identity salt). Every prop must be a
// literal of the documented form; anything else is TESSERA_CODEGEN_UNSUPPORTED naming the prop.
[[nodiscard]] bool build_vector_payload(std::string_view component_name, const ir::Node& node,
                                        unsigned ordinal, VectorPayload& payload,
                                        std::string& error);

// Walks every component of `module` and collects the synthesized asset of every `Path` node, sorted
// by asset name (the bundle's lookup invariant). Runs before any file is emitted, so a rejected
// prop produces no partial file set.
[[nodiscard]] bool collect_vector_assets(const ir::IrModule& module,
                                         std::vector<VectorAsset>& assets, std::string& error);

// Emits `<module.name>_vector_assets.hpp`: the SVG bytes as `inline constexpr` arrays, the sorted
// `AssetEntry` table and an inline `vector_assets()` accessor returning the `AssetBundle` the paint
// sink's `VectorPaintConfig::bundle` takes. Deterministic: same assets, same bytes.
[[nodiscard]] std::string emit_vector_assets_header(std::string_view module_name,
                                                    std::span<const VectorAsset> assets);

// Emits the `PaintData` of one `Path` node (`kind = Vector`, `vector.asset = "<name>"`).
[[nodiscard]] bool emit_vector_paint(CppWriter& writer, const ir::Node& node,
                                     std::string_view component_name, unsigned ordinal,
                                     std::string_view variable, std::string& error);

// Emits the `label` prop as the node's accessible name (`aria-label`), the channel the
// accessibility tree resolves a name from. A non-literal label is rejected loudly.
[[nodiscard]] bool emit_vector_label(CppWriter& writer, const ir::Node& node,
                                     std::string_view variable, std::string& error);

} // namespace tessera::compiler::codegen::internal
