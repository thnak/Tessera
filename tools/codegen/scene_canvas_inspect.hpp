// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/style/cascade.hpp"

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

// One designer override. Empty for this slice: overrides are `style.override.set`
// (designer-protocol-v1 section 9.2), a later slice, and an empty list is the correct answer for a
// node that has none.
struct StyleOverrideInfo {
    std::string property;
    std::string value;
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

} // namespace tessera::compiler::codegen::cli
