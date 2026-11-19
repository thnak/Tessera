// SPDX-License-Identifier: MIT

#pragma once

#include "scene_override.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What the designer's `style.info` answers for one node (designer-protocol-v1 section 9.1), as the
// engine computes it. The report serializer (`canvas_report.cpp`) turns this into the document the
// page parses; this header is the value, so a test can assert the cascade's answer without reading
// JSON.
//
// **Why the engine answers this and not the page.** Every field is a fact only the cascade has:
// `computed` is the resolved style after inheritance and `var()`, `matched` is the selector index's
// own answer with the specificity it matched at, and `overridden` is the cascade's own comparison.
// A page that recomputed any of it would be a second cascade, and the first time the two disagreed
// the inspector would describe a style the canvas is not painting.
namespace tessera::compiler::codegen::cli {

// One declaration of one matched rule, as the inspector lists it.
struct StyleDeclarationInfo {
    std::string property;
    std::string value;
    bool important = false;
    // True when a higher-precedence declaration won this property, computed by the cascade's own
    // `cascades_over` over the same keys the cascade used - never guessed from source order.
    bool overridden = false;
};

// One rule that matched the inspected node.
struct StyleMatchedRule {
    // `"inline"`, `"author"`, `"theme"` or `"ua"` (stylesheet-v1 section 7.1).
    std::string_view origin;
    // The selector list as text, as the author wrote it when the sheet was prepared from source.
    std::string selector;
    // `[ids, classes, types]`.
    std::array<std::uint32_t, 3> specificity{};
    // The rule's SourceSpan (designer-protocol-v1 section 3.1). `file` is the sheet the rule came
    // from; `offset` is the rule's byte offset in it. A rule resolved from a blob has no authored
    // text and no source file, so both are empty/0.
    std::string file;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::vector<StyleDeclarationInfo> declarations;
};

// The node's layout box in logical pixels. `margin`/`border`/`padding` are top, right, bottom,
// left.
struct StyleBoxInfo {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    std::array<float, 4> margin{};
    std::array<float, 4> border{};
    std::array<float, 4> padding{};
};

// One designer override (designer-protocol-v1 section 9.2): a property the developer set on a node
// in the override layer, with the value as typed. An empty list is the correct answer for a node
// that has none.
struct StyleOverrideInfo {
    // The node it belongs to. `style.info` is about one node and does not print it; the override
    // operations answer for several nodes at once (`revert`, `undo`) and do.
    std::uint64_t automation_id = 0;
    std::string property;
    std::string value;
};

// The answer to one override operation (designer-protocol-v1 sections 9.2-9.4).
struct OverrideOutcome {
    // False when the operation was refused: `diagnostics` says why for an invalid declaration
    // (a `TESSERA_CSS_*` code, or `TESSERA_DESIGNER_LIMIT`), and `reason` is `no_such_node` when
    // the NodeRef names no node. A refusal changes nothing.
    bool ok = false;
    std::string reason;
    std::vector<OverrideDiagnostic> diagnostics;
    // True when the layer changed and a frame is owed; false for a no-op (the same value set twice,
    // a clear of nothing, an undo with an empty history).
    bool changed = false;
    // The layer after the operation: the named node's overrides for `set`/`clear`, every node's for
    // `revert`/`undo`/`redo`.
    std::vector<StyleOverrideInfo> overrides;
    bool can_undo = false;
    bool can_redo = false;
};

// The answer to settling a commit (designer-protocol-v1 section 10.3).
struct CommitSettleResult {
    // False when the node is not in the running evaluation (`reason` `no_such_node`) or its
    // overrides did not round-trip (`diagnostics` holds `TESSERA_DESIGNER_PATCH_MISMATCH`).
    bool ok = false;
    std::string reason;
    // True when the node's overrides were consumed: they left the layer and the history.
    bool consumed = false;
    std::vector<OverrideDiagnostic> diagnostics;
    // What the layer still holds for the node (everything of the node's, on a mismatch).
    std::vector<StyleOverrideInfo> overrides;
    // Whether the override history still has a step to undo or redo AFTER the settle. A consumed
    // node's operations leave the history, so the page cannot derive these from its own copy.
    bool can_undo = false;
    bool can_redo = false;
};

// The whole answer. `found:false` is `no_such_node`: the NodeRef names no node in the running
// evaluation, and the report carries no other field (designer-protocol-v1 section 9.1).
struct StyleInfoResult {
    bool found = false;
    std::uint64_t automation_id = 0;
    // Property -> resolved value, for every property with a non-initial value.
    std::vector<std::pair<std::string, std::string>> computed;
    // Highest precedence first.
    std::vector<StyleMatchedRule> matched;
    StyleBoxInfo box;
    std::vector<StyleOverrideInfo> overrides;
};

// The node of `root`'s subtree carrying `automation_id`, or null. 0 is "no automation id" and so
// never names a node. Depth-first in tree order, the order the inspector and the picker agree on.
[[nodiscard]] const tessera::ui::Node* find_node_by_automation_id(const tessera::ui::Node& root,
                                                                  std::uint64_t automation_id);

// The style inspector's answer for one node of a laid-out tree (designer-protocol-v1 section 9.1):
// computed style, matched rules and the layout box. It reads the tree and the sheet only, so it is
// a query - and the one function behind both the studio's `style.info` and the MCP `query_style`.
// `overrides` is the session's and is left empty here. `found:false` for an id no node carries.
[[nodiscard]] StyleInfoResult style_info_for(const tessera::ui::Node& root,
                                             const tessera::ui::style::CompiledStyleSheet& sheet,
                                             std::uint64_t automation_id);

} // namespace tessera::compiler::codegen::cli
