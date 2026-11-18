// SPDX-License-Identifier: MIT
//
// The style inspector's engine half (issue #171, designer-protocol-v1 section 9.1): for one node,
// the computed style, the matched rules with their provenance, and the box.
//
// The acceptance sentence has five clauses, and each is asserted here:
//
//  1. **The right node's computed values.** The values reported for a node are the ones the
//     cascade resolves for that node - read back through the same `SceneStyleTree::compute()` the
//     paint pass uses, so a passing assertion is evidence the session reported the tree rather than
//     a constant.
//  2. **`matched` is highest precedence first.** A rule that wins a property appears before the
//     rule it beat, and the order is the cascade's own total order, not source order.
//  3. **`overridden` is the cascade's answer.** A declaration beaten by a more specific rule is
//     marked, and the winner is not - computed from the cascade keys, never guessed.
//  4. **The box is the node's own.** `world_bounds()` field by field, plus the three edge sets.
//  5. **`no_such_node`.** A missing id (and the engine's "no id" 0) answers `found:false` with no
//     other field.
//
// The report serializer is asserted too, because the page parses it: the document is one line, a
// miss carries `ok:false` and `reason` and nothing else, and the box is four numbers plus three
// four-number arrays.

#include "scene_canvas_test_support.hpp"

#include <string>

namespace {

using namespace canvas_test;

// A source whose two rules differ only in specificity, so the cascade has a real winner and a real
// loser for the same property. `#target` (one id) beats `.card` (one class) whatever the source
// order, which is what makes the `overridden` assertion meaningful rather than incidental.
//
// The `import "./Preview.css";` is load-bearing: the preview applies the sheets a program imports,
// as a native build would, so a source that never imports it previews with no author CSS at all.
constexpr std::string_view kSpecificity = R"tsx(import "./Preview.css";

export function Specificity(): Node {
  return (
    <View className="card" id="target">
      <Text>Hello</Text>
    </View>
  );
}
)tsx";

constexpr std::string_view kSpecificityCss = R"(.card { background-color: #112233; color: #ffffff; }
#target { background-color: #445566; }
)";

cli::CanvasSessionSource source_with_css(std::string_view text, std::string_view file,
                                         std::string_view css) {
    cli::CanvasSessionSource source = source_for(text, file);
    source.stylesheet = css;
    source.stylesheet_file = "ui/Preview.css";
    return source;
}

// The computed value of `property` for `node`, through the engine's own cascade: the test's own
// reading of the tree, so a passing assertion is evidence the session reported the tree.
std::string computed_of(const cli::CanvasSceneSession& session, const ui::Node& node,
                        std::string_view property) {
    ui::style::SceneStyleTree tree(*session.root());
    const ui::style::ComputedStyle style = tree.compute(session.sheet(), node);
    const std::string* value = style.find(property);
    return value != nullptr ? *value : std::string();
}

// The inspector's answer for `property`, or an empty string when it did not report it.
std::string reported(const cli::StyleInfoResult& result, std::string_view property) {
    for (const auto& [name, value] : result.computed) {
        if (name == property) {
            return value;
        }
    }
    return {};
}

// The index of the first matched rule whose selector is `selector`, or `npos`.
std::size_t rule_index(const cli::StyleInfoResult& result, std::string_view selector) {
    for (std::size_t index = 0; index < result.matched.size(); ++index) {
        if (result.matched[index].selector == selector) {
            return index;
        }
    }
    return static_cast<std::size_t>(-1);
}

void test_computed_matches_the_cascade() {
    cli::CanvasSceneSession session;
    expect(
        session.start(source_with_css(kSpecificity, "ui/Specificity.tsx", kSpecificityCss)).empty(),
        "the specificity source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    expect(card != nullptr, "the source has a .card node");
    if (card == nullptr) {
        return;
    }

    const cli::StyleInfoResult info = session.inspect(card->automation_id());
    expect(info.found, "the inspector finds the node by its automation id");
    expect(info.automation_id == card->automation_id(), "the answer names the node it inspected");

    // The winner of `background-color` is `#target`, so the computed value is that rule's, and the
    // test reads the same value out of the cascade rather than hard-coding the hex.
    const std::string expected = computed_of(session, *card, "background-color");
    expect(!expected.empty(), "the cascade resolves a background-color for the card");
    expect(reported(info, "background-color") == expected,
           "the reported background-color is the cascade's, got `" +
               reported(info, "background-color") + "` expected `" + expected + "`");
    // A property no rule sets is not reported: the spec says "every property with a non-initial
    // value", and the registry's initial value is what an unmatched property resolves to.
    expect(reported(info, "border-top-width").empty(),
           "a property at its initial value is not reported");

    // A node with no rule of its own still answers: the inspector is not limited to styled nodes.
    const ui::Node* text = find_kind(*session.root(), "Text");
    expect(text != nullptr, "the source has a text node");
    if (text != nullptr) {
        const cli::StyleInfoResult text_info = session.inspect(text->automation_id());
        expect(text_info.found, "the inspector finds an unstyled node too");
        expect(reported(text_info, "color") == computed_of(session, *text, "color"),
               "an inherited property is reported as the cascade resolved it");
    }
}

void test_matched_is_highest_precedence_first() {
    cli::CanvasSceneSession session;
    expect(
        session.start(source_with_css(kSpecificity, "ui/Specificity.tsx", kSpecificityCss)).empty(),
        "the specificity source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "the source has a .card node");
        return;
    }
    const cli::StyleInfoResult info = session.inspect(card->automation_id());
    const std::size_t id_rule = rule_index(info, "#target");
    const std::size_t class_rule = rule_index(info, ".card");
    expect(id_rule != static_cast<std::size_t>(-1), "the #target rule is in `matched`");
    expect(class_rule != static_cast<std::size_t>(-1), "the .card rule is in `matched`");
    if (id_rule == static_cast<std::size_t>(-1) || class_rule == static_cast<std::size_t>(-1)) {
        return;
    }
    // `#target` has one id and `.card` one class, so the id rule wins `background-color` and must
    // come first. This is the spec's "highest precedence first", asserted against the cascade's own
    // ordering rather than against the source order (which is the opposite here).
    expect(id_rule < class_rule, "the more specific rule is listed first, got #target at " +
                                     std::to_string(id_rule) + " and .card at " +
                                     std::to_string(class_rule));
    expect(info.matched[id_rule].specificity[0] == 1 && info.matched[id_rule].specificity[1] == 0,
           "the #target rule reports specificity [1,0,0]");
    expect(info.matched[class_rule].specificity[1] == 1,
           "the .card rule reports one class in its specificity");
    // The provenance the slice exists for: the selector text a developer would recognise, and the
    // origin the cascade used.
    expect(info.matched[id_rule].origin == "author", "the rule's origin is the author sheet");
    expect(!info.matched[id_rule].selector.empty(), "the rule carries its selector text");
}

void test_overridden_is_the_cascade_answer() {
    cli::CanvasSceneSession session;
    expect(
        session.start(source_with_css(kSpecificity, "ui/Specificity.tsx", kSpecificityCss)).empty(),
        "the specificity source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "the source has a .card node");
        return;
    }
    const cli::StyleInfoResult info = session.inspect(card->automation_id());
    const std::size_t id_rule = rule_index(info, "#target");
    const std::size_t class_rule = rule_index(info, ".card");
    if (id_rule == static_cast<std::size_t>(-1) || class_rule == static_cast<std::size_t>(-1)) {
        expect(false, "both rules are in `matched`");
        return;
    }

    // `.card` sets `background-color` and `#target` beats it, so the `.card` declaration is
    // overridden and the `#target` one is not. `color` is set only by `.card`, so it is not
    // overridden: the flag is per declaration, not per rule.
    bool class_background_overridden = false;
    bool class_color_overridden = true;
    for (const cli::StyleDeclarationInfo& declaration : info.matched[class_rule].declarations) {
        if (declaration.property == "background-color") {
            class_background_overridden = declaration.overridden;
        }
        if (declaration.property == "color") {
            class_color_overridden = declaration.overridden;
        }
    }
    expect(class_background_overridden,
           "the .card background-color is marked overridden by the more specific #target rule");
    expect(!class_color_overridden, "the .card color is not overridden: no other rule sets it");

    bool id_background_overridden = true;
    for (const cli::StyleDeclarationInfo& declaration : info.matched[id_rule].declarations) {
        if (declaration.property == "background-color") {
            id_background_overridden = declaration.overridden;
        }
    }
    expect(!id_background_overridden, "the winning #target declaration is not overridden");
}

void test_box_is_the_nodes_own() {
    cli::CanvasSceneSession session;
    expect(
        session.start(source_with_css(kSpecificity, "ui/Specificity.tsx", kSpecificityCss)).empty(),
        "the specificity source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "the source has a .card node");
        return;
    }
    const cli::StyleInfoResult info = session.inspect(card->automation_id());
    const rhi::Rect bounds = card->world_bounds();
    expect(info.box.x == bounds.x, "the box x is the node's own world_bounds x");
    expect(info.box.y == bounds.y, "the box y is the node's own world_bounds y");
    expect(info.box.width == bounds.width, "the box width is the node's own world_bounds width");
    expect(info.box.height == bounds.height,
           "the box height is the node's own world_bounds height");
    // The edge sets are four numbers each, in top/right/bottom/left order. The built-in sheet gives
    // a View no padding, so the card's are zero - asserted as "the cascade's value", not as a
    // constant, so a sheet change moves the test with it.
    expect(info.box.padding.size() == 4 && info.box.margin.size() == 4 &&
               info.box.border.size() == 4,
           "the three edge sets are four numbers each");
    const std::string padding_top = computed_of(session, *card, "padding-top");
    const float expected_top = padding_top.empty() ? 0.0f : std::stof(padding_top);
    expect(info.box.padding[0] == expected_top, "the padding top is the cascade's padding-top");
}

void test_missing_node_is_no_such_node() {
    cli::CanvasSceneSession session;
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "the counter starts");

    // An id no node carries. `999999` is far above any id the preview assigns, and the assertion is
    // about the answer, not the number.
    const cli::StyleInfoResult missing = session.inspect(999999);
    expect(!missing.found, "an id no node carries answers found:false");
    expect(missing.computed.empty() && missing.matched.empty(),
           "a missing node reports no computed style and no matched rules");
    expect(missing.box.width == 0.0f && missing.box.height == 0.0f,
           "a missing node reports a zero box");

    // `0` is the engine's "no automation id" (automation-ids-v1 section 1), so it can never name a
    // node: a NodeRef of 0 is `no_such_node`, not the first node in the tree.
    const cli::StyleInfoResult zero = session.inspect(0);
    expect(!zero.found, "id 0 answers found:false");

    // A session with no tree answers the same way rather than crashing: the page opens the
    // inspector before the first Run, and a null tree is the ordinary state at that moment.
    cli::CanvasSceneSession empty;
    expect(!empty.inspect(1).found, "an unstarted session inspects nothing");
}

void test_style_report_json() {
    cli::StyleInfoResult result;
    result.found = true;
    result.automation_id = 101;
    result.computed.emplace_back("background-color", "#445566");
    cli::StyleMatchedRule rule;
    rule.origin = "author";
    rule.selector = "#target";
    rule.specificity = {1, 0, 0};
    rule.file = "ui/Preview.css";
    rule.offset = 42;
    rule.declarations.push_back(
        cli::StyleDeclarationInfo{"background-color", "#445566", false, false});
    result.matched.push_back(rule);
    result.box.x = 12.0f;
    result.box.y = 40.0f;
    result.box.width = 120.0f;
    result.box.height = 32.0f;
    result.box.padding = {8.0f, 8.0f, 8.0f, 8.0f};

    const std::string report = cli::canvas_style_report_json(result);
    expect(report.find("\"styleVersion\":1") != std::string::npos,
           "the style report carries its version");
    expect(report.find("\"ok\":true") != std::string::npos, "the report says the node was found");
    expect(report.find("\"node\":\"101\"") != std::string::npos,
           "the id is a quoted decimal string");
    expect(report.find("\"computed\":{\"background-color\":\"#445566\"}") != std::string::npos,
           "computed is an object of property -> value, got " + report);
    expect(report.find("\"selector\":\"#target\"") != std::string::npos,
           "the matched rule carries its selector text");
    expect(report.find("\"specificity\":[1,0,0]") != std::string::npos,
           "specificity is [ids, classes, types]");
    expect(report.find("\"span\":{\"file\":\"ui/Preview.css\",\"offset\":42,\"length\":0}") !=
               std::string::npos,
           "the span names the file and the rule's offset, got " + report);
    expect(report.find("\"overridden\":false") != std::string::npos,
           "a declaration carries its overridden flag");
    expect(report.find("\"box\":{\"x\":12,\"y\":40,\"width\":120,\"height\":32,") !=
               std::string::npos,
           "the box is four numbers in logical pixels, got " + report);
    expect(report.find("\"padding\":[8,8,8,8]") != std::string::npos,
           "the padding is a four-number array");
    expect(report.find("\"overrides\":[]") != std::string::npos,
           "the overrides list is present and empty for this slice");
    expect(report.back() == '\n', "the report is one newline-terminated line");

    // A miss is the spec's whole answer: `ok:false`, the reason, and nothing else. A page that read
    // a stale `computed` off a failed answer would show the previous node's style under the new
    // node's name.
    const std::string miss = cli::canvas_style_report_json(cli::StyleInfoResult{});
    expect(miss.find("\"ok\":false") != std::string::npos, "a miss reports ok:false");
    expect(miss.find("\"reason\":\"no_such_node\"") != std::string::npos,
           "a miss names no_such_node");
    expect(miss.find("\"computed\"") == std::string::npos,
           "a miss carries no computed style, got " + miss);
    expect(miss.find("\"box\"") == std::string::npos, "a miss carries no box");
    expect(miss.back() == '\n', "a miss is one newline-terminated line");
}

void test_empty_rule_block_is_not_invented() {
    // `.card { }` is legal CSS. The preparer drops a rule whose block expands to no declarations
    // (`prepared_stylesheet.cpp`), because such a rule cannot affect the cascade - so the inspector
    // must not invent it either. What it must do is report the rule that *does* set the property,
    // and not mark it overridden by a rule that sets nothing.
    constexpr std::string_view kEmpty = R"tsx(import "./Preview.css";

export function Empty(): Node {
  return (
    <View className="card">
      <Text>Hello</Text>
    </View>
  );
}
)tsx";
    constexpr std::string_view kEmptyCss = ".card { }\n.card { color: #ffffff; }\n";
    cli::CanvasSceneSession session;
    expect(session.start(source_with_css(kEmpty, "ui/Empty.tsx", kEmptyCss)).empty(),
           "the empty-block source starts");
    const ui::Node* card = find_class(*session.root(), "card");
    if (card == nullptr) {
        expect(false, "the source has a .card node");
        return;
    }
    const cli::StyleInfoResult info = session.inspect(card->automation_id());
    expect(info.found, "the inspector finds a node matched by an empty rule");
    expect(info.matched.size() == 1, "only the rule that sets a property is listed, got " +
                                         std::to_string(info.matched.size()));
    for (const cli::StyleMatchedRule& rule : info.matched) {
        expect(!rule.declarations.empty(), "a listed rule has at least one declaration");
        for (const cli::StyleDeclarationInfo& declaration : rule.declarations) {
            if (declaration.property == "color") {
                expect(!declaration.overridden,
                       "the color is not overridden: the empty rule sets nothing");
            }
        }
    }
}

} // namespace

int main() {
    test_computed_matches_the_cascade();
    test_matched_is_highest_precedence_first();
    test_overridden_is_the_cascade_answer();
    test_box_is_the_nodes_own();
    test_missing_node_is_no_such_node();
    test_empty_rule_block_is_not_invented();
    test_style_report_json();
    if (failures() != 0) {
        std::cerr << "scene_canvas_style: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_style: ok\n";
    return 0;
}
