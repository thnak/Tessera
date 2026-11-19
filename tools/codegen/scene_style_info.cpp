// SPDX-License-Identifier: MIT

#include "scene_canvas_inspect.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The style inspector's query over a laid-out tree and its sheet. It is its own translation unit,
// separate from `CanvasSceneSession::inspect`, so the stateless callers (the MCP `query_style`
// tool) link it without the session's GPU/paint stack: one cascade query, however it is reached.
namespace tessera::compiler::codegen::cli {
namespace {

namespace style = tessera::ui::style;
namespace rhi = tessera::rendering::rhi;
namespace ui = tessera::ui;

// The spec's origin vocabulary (stylesheet-v1 section 7.1, designer-protocol-v1 section 9.1). The
// engine's `Origin::User` has no designer spelling because the preview never compiles a user sheet;
// it is reported as `"author"` rather than invented, so a page never sees a word the spec does not
// define.
std::string_view origin_name(style::Origin origin) {
    switch (origin) {
    case style::Origin::Inline:
        return "inline";
    case style::Origin::Theme:
        return "theme";
    case style::Origin::UserAgent:
        return "ua";
    case style::Origin::User:
    case style::Origin::Author:
        break;
    }
    return "author";
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// A px length from a computed value, or nullopt for a unit that needs a containing block
// (`%`, `em`, `vw`). The box report is in logical pixels, so a value the engine cannot resolve
// without layout is reported as 0 rather than as a guess: the inspector must not print a number the
// canvas is not painting.
std::optional<float> px_value(const style::ComputedStyle& computed, const char* property) {
    const std::string* text = computed.find(property);
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string_view token = trim(*text);
    if (token.empty()) {
        return std::nullopt;
    }
    if (token.size() > 2 && token.substr(token.size() - 2) == "px") {
        token = trim(token.substr(0, token.size() - 2));
    } else if (std::isalpha(static_cast<unsigned char>(token.back())) != 0 || token.back() == '%') {
        return std::nullopt;
    }
    const std::string number(token);
    char* end = nullptr;
    const float parsed = std::strtof(number.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

// One edge set in top, right, bottom, left order, the order the spec's box arrays use.
std::array<float, 4> edges(const style::ComputedStyle& computed, const char* top, const char* right,
                           const char* bottom, const char* left) {
    return std::array<float, 4>{
        px_value(computed, top).value_or(0.0f), px_value(computed, right).value_or(0.0f),
        px_value(computed, bottom).value_or(0.0f), px_value(computed, left).value_or(0.0f)};
}

// A border side contributes only when its style paints it, which is the rule paint uses
// (`paint::detail::border_width`): a `border-width: 4px` with no `border-style` paints nothing, so
// reporting 4 would describe a box the canvas does not draw.
float border_side(const style::ComputedStyle& computed, const char* width, const char* style_name) {
    const std::string* style_text = computed.find(style_name);
    if (style_text == nullptr) {
        return 0.0f;
    }
    const std::string keyword = lower(trim(*style_text));
    if (keyword.empty() || keyword == "none" || keyword == "hidden") {
        return 0.0f;
    }
    return px_value(computed, width).value_or(0.0f);
}

// The cascade key of one declaration of one matched rule. `source_order` is the rule's base plus
// the declaration's index, which is exactly what `CascadeTable::add_declarations` assigns, so the
// comparison below is the cascade's own and not a second rule that could disagree with it.
style::CascadeKey declaration_key(const style::CompiledRule& rule,
                                  const style::Specificity& specificity, std::size_t index,
                                  bool important) {
    return style::CascadeKey{rule.origin, important, specificity, rule.source_order + index,
                             rule.layer};
}

// The key that orders one rule against another. A rule's declarations share its origin, layer and
// specificity, so the rule's own precedence is that key at its first declaration. Importance is
// passed in rather than read out of a declaration because the key must be buildable for a rule with
// no declarations: the preparer drops an empty block today, so `matched` never holds one, but a
// future prepare pass that keeps them must not turn this into an out-of-bounds read.
style::CascadeKey rule_key(const style::CompiledRule& rule, const style::Specificity& specificity) {
    return declaration_key(rule, specificity, 0, false);
}

// Which declaration won each property, by the cascade's own comparison. The winner is kept as
// (rule, declaration) so `overridden` is an identity test rather than a re-derivation: a
// declaration is overridden exactly when it is not the one the cascade kept.
struct Winner {
    std::size_t rule = 0;
    std::size_t declaration = 0;
};

std::map<std::string, Winner> winners_of(const std::vector<const style::CompiledRule*>& rules,
                                         const std::vector<style::Specificity>& specificities) {
    std::map<std::string, Winner> winners;
    std::map<std::string, style::CascadeKey> keys;
    for (std::size_t r = 0; r < rules.size(); ++r) {
        const style::CompiledRule& rule = *rules[r];
        for (std::size_t d = 0; d < rule.declarations.size(); ++d) {
            const std::string property = style::normalize_property(rule.declarations[d].property);
            const style::CascadeKey key =
                declaration_key(rule, specificities[r], d, rule.declarations[d].important);
            const auto found = keys.find(property);
            if (found == keys.end() || style::cascades_over(key, found->second)) {
                keys[property] = key;
                winners[property] = Winner{r, d};
            }
        }
    }
    return winners;
}

} // namespace

const ui::Node* find_node_by_automation_id(const ui::Node& node, std::uint64_t automation_id) {
    if (node.automation_id() == automation_id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_node_by_automation_id(*child, automation_id)) {
            return found;
        }
    }
    return nullptr;
}

StyleInfoResult style_info_for(const ui::Node& root, const style::CompiledStyleSheet& sheet,
                               std::uint64_t automation_id) {
    StyleInfoResult result;
    // `0` is the engine's "no automation id" (automation-ids-v1 section 1), so it can never name a
    // node: a NodeRef of 0 is `no_such_node`, not the first node in the tree.
    if (automation_id == 0) {
        return result;
    }
    const ui::Node* node = find_node_by_automation_id(root, automation_id);
    if (node == nullptr) {
        return result;
    }
    result.found = true;
    result.automation_id = automation_id;

    // The cascade and the computed style, through the engine's own tree view: the same call the
    // paint pass makes, so the values reported here are the values being painted.
    style::SceneStyleTree tree(root);
    const style::ComputedStyle computed = tree.compute(sheet, *node);
    const style::PropertyRegistry& registry = sheet.registry();
    for (const auto& [property, value] : computed.properties()) {
        const style::PropertyInfo* info = registry.find(property);
        // "Every property with a non-initial value" (designer-protocol-v1 section 9.1): the
        // computed style carries the registry's initial value for everything unmatched, and a list
        // of those would bury the properties the developer actually set. A custom property has no
        // registry entry and is always reported.
        if (info != nullptr && info->initial == value) {
            continue;
        }
        result.computed.emplace_back(property, value);
    }

    // The matched rules, highest precedence first. `cascades_over` is the cascade's total order on
    // (importance, origin, layer, specificity, source order), so sorting by it puts the rule that
    // wins first - which is what the spec's "highest precedence first" means.
    const style::StyleNode* view = tree.view(*node);
    std::vector<const style::CompiledRule*> rules;
    std::vector<style::Specificity> specificities;
    if (view != nullptr) {
        for (const style::CompiledStyleSheet::MatchedRule& match : sheet.matched_rules(*view)) {
            rules.push_back(match.rule);
            specificities.push_back(match.specificity);
        }
    }
    std::vector<std::size_t> order(rules.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return style::cascades_over(rule_key(*rules[a], specificities[a]),
                                    rule_key(*rules[b], specificities[b]));
    });

    const std::map<std::string, Winner> winners = winners_of(rules, specificities);
    for (const std::size_t index : order) {
        const style::CompiledRule& rule = *rules[index];
        StyleMatchedRule entry;
        entry.origin = origin_name(rule.origin);
        entry.selector = rule.selector;
        entry.specificity = {specificities[index].ids, specificities[index].classes,
                             specificities[index].types};
        entry.file = std::string(rule.source_path);
        entry.offset = static_cast<std::uint32_t>(rule.offset);
        for (std::size_t d = 0; d < rule.declarations.size(); ++d) {
            const style::Declaration& declaration = rule.declarations[d];
            StyleDeclarationInfo info;
            info.property = declaration.property;
            info.value = declaration.value;
            info.important = declaration.important;
            const auto winner = winners.find(style::normalize_property(declaration.property));
            info.overridden = winner != winners.end() &&
                              !(winner->second.rule == index && winner->second.declaration == d);
            entry.declarations.push_back(std::move(info));
        }
        result.matched.push_back(std::move(entry));
    }

    // The box: the node's own world bounds, plus the three edge sets the spec asks for. The bounds
    // are the border box, which is what `world_bounds()` is and what the picker already reports.
    const rhi::Rect bounds = node->world_bounds();
    result.box.x = bounds.x;
    result.box.y = bounds.y;
    result.box.width = bounds.width;
    result.box.height = bounds.height;
    result.box.margin =
        edges(computed, "margin-top", "margin-right", "margin-bottom", "margin-left");
    result.box.padding =
        edges(computed, "padding-top", "padding-right", "padding-bottom", "padding-left");
    result.box.border = {border_side(computed, "border-top-width", "border-top-style"),
                         border_side(computed, "border-right-width", "border-right-style"),
                         border_side(computed, "border-bottom-width", "border-bottom-style"),
                         border_side(computed, "border-left-width", "border-left-style")};

    // The `overrides` list is the session's (`CanvasSceneSession::inspect` fills it); this function
    // reads the tree and the sheet only, so an empty list is what it answers.
    return result;
}

} // namespace tessera::compiler::codegen::cli
