// SPDX-License-Identifier: MIT
//
// `::part()` (issue #235, docs/specs/stylesheet-v1.md section 6.3, ADR-047 item 3): selector
// parsing, specificity, matching across the component boundary, the cascade on a real scene, the
// undeclared-part diagnostic, `@supports selector()` and the compiled-stylesheet round trip.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/component_parts.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/selector.hpp"
#include "tessera/ui/style/selector_match.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/supports.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::style::check_part_selectors;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::ComponentPartRegistry;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::load_stylesheet_blob;
using tessera::ui::style::Origin;
using tessera::ui::style::parse_selector_list;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PseudoClass;
using tessera::ui::style::SceneStyleTree;
using tessera::ui::style::SelectorList;
using tessera::ui::style::SelectorMatch;
using tessera::ui::style::SelectorParseResult;
using tessera::ui::style::serialize_stylesheet;
using tessera::ui::style::Specificity;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::style::StyleNode;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "part_pseudo: " << message << "\n";
    }
    return condition;
}

const PseudoClass* first_pseudo(const SelectorParseResult& parsed) {
    if (!parsed.ok || parsed.list.selectors.empty()) {
        return nullptr;
    }
    const auto& compound = parsed.list.selectors.front().compounds;
    if (compound.empty() || compound.front().pseudos.empty()) {
        return nullptr;
    }
    return &compound.front().pseudos.front();
}

bool supported(std::string_view text) {
    return tessera::ui::style::selector_supported(text);
}

bool supports(std::string_view text) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, tessera::ui::style::PropertyRegistry{});
}

// --- selector parsing -------------------------------------------------------------------------

bool parsing() {
    bool ok = true;

    const SelectorParseResult simple = parse_selector_list("Button::part(label)");
    const PseudoClass* pseudo = first_pseudo(simple);
    ok = expect(simple.ok && pseudo != nullptr, "`Button::part(label)` parses") && ok;
    ok = expect(pseudo != nullptr && pseudo->name == "part", "the pseudo-element is `part`") && ok;
    ok =
        expect(pseudo != nullptr && pseudo->argument == "label", "the part name is the argument") &&
        ok;
    ok = expect(pseudo != nullptr && pseudo->pseudo_element, "`::part` is a pseudo-element") && ok;
    ok = expect(!simple.list.selectors.empty() &&
                    simple.list.selectors.front().compounds.front().tag == "Button",
                "the host-side type selector is kept") &&
         ok;

    // `.cls::part(name):hover`: the class belongs to the host, the pseudo-class to the part.
    const SelectorParseResult styled = parse_selector_list(".cls::part(name):hover");
    tessera::ui::style::CompoundSelector styled_compound;
    if (styled.ok && !styled.list.selectors.empty() &&
        !styled.list.selectors.front().compounds.empty()) {
        styled_compound = styled.list.selectors.front().compounds.front();
    }
    ok = expect(styled.ok && styled_compound.classes.size() == 1 &&
                    styled_compound.classes.front() == "cls",
                "a host-side class parses") &&
         ok;
    ok = expect(styled_compound.pseudos.size() == 2 && styled_compound.pseudos[0].name == "part" &&
                    styled_compound.pseudos[1].name == "hover",
                "the part pseudo-element precedes the part-side pseudo-class") &&
         ok;

    // `::part(<ident>+)`: several names, in either order, must all be present.
    const SelectorParseResult multiple = parse_selector_list("x::part(tab active)");
    const PseudoClass* names = first_pseudo(multiple);
    ok = expect(names != nullptr && names->argument == "tab active",
                "`::part(tab active)` keeps both names") &&
         ok;

    // Malformed forms are parse errors rather than selectors that silently never match.
    for (const std::string_view text :
         {"::part()", "::part", "::part(2x)", "::part(\"label\")", "::part(label)!",
          "::part(label)::part(other)", "::part(label)::before", ".a::part(label) .b"}) {
        ok = expect(!parse_selector_list(text).ok,
                    std::string("`") + std::string(text) + "` is rejected") &&
             ok;
    }
    return ok;
}

// --- specificity ------------------------------------------------------------------------------

bool specificity() {
    bool ok = true;
    const auto of = [](std::string_view text) {
        const SelectorParseResult parsed = parse_selector_list(text);
        return tessera::ui::style::specificity(parsed.list);
    };
    ok = expect(of("::part(label)") == Specificity{0, 0, 1},
                "`::part()` contributes one pseudo-element, its name adds nothing") &&
         ok;
    ok = expect(of("Button::part(label)") == Specificity{0, 0, 2},
                "the host type selector adds a type") &&
         ok;
    ok = expect(of(".cls::part(name):hover") == Specificity{0, 2, 1},
                "the host class and the part-side pseudo-class are classes") &&
         ok;
    ok = expect(of("#id::part(label)") == Specificity{1, 0, 1}, "an id is an id") && ok;
    ok = expect(of(".card::part(label)") < of(".card.primary::part(label)"),
                "a more specific host selector wins") &&
         ok;
    // `:where()` still contributes nothing, including when it precedes `::part()`.
    ok = expect(of(":where(.card)::part(label)") == of("::part(label)"),
                "`:where()` contributes zero specificity") &&
         ok;
    return ok;
}

// --- matching across the component boundary ---------------------------------------------------

struct Scene {
    StyleNode page;
    StyleNode host;      // the `Card` component instance: the boundary
    StyleNode card_root; // the component's own root element
    StyleNode label;     // part="label"
    StyleNode nested;    // a second Card instance inside the first one
    StyleNode nested_root;
    StyleNode nested_label; // part="label" inside the nested component
    StyleNode loose;        // part="loose", outside every component
};

void attach(StyleNode& parent, StyleNode& child) {
    parent.children.push_back(&child);
    child.parent = &parent;
    child.host = parent.component_host ? &parent : parent.host;
}

void build(Scene& scene) {
    scene.page.tag = "View";
    scene.page.classes = {"page"};
    scene.host.tag = "Card";
    scene.host.component_host = true;
    scene.host.classes = {"card"};
    scene.card_root.tag = "View";
    scene.label.tag = "Text";
    scene.label.part = "label";
    scene.nested.tag = "Card";
    scene.nested.component_host = true;
    scene.nested.classes = {"other"};
    scene.nested_root.tag = "View";
    scene.nested_label.tag = "Text";
    scene.nested_label.part = "label";
    scene.loose.tag = "Text";
    scene.loose.part = "loose";

    attach(scene.page, scene.host);
    attach(scene.host, scene.card_root);
    attach(scene.card_root, scene.label);
    attach(scene.card_root, scene.nested);
    attach(scene.nested, scene.nested_root);
    attach(scene.nested_root, scene.nested_label);
    attach(scene.page, scene.loose);
}

bool matches_text(std::string_view text, const StyleNode& node) {
    const SelectorParseResult parsed = parse_selector_list(text);
    return parsed.ok && tessera::ui::style::matches(parsed.list, node);
}

bool matching() {
    Scene scene;
    build(scene);
    bool ok = true;

    ok = expect(matches_text("Card::part(label)", scene.label),
                "the host-side type selector names the component") &&
         ok;
    ok = expect(matches_text(".card::part(label)", scene.label),
                "the host-side class matches the component instance") &&
         ok;
    ok = expect(!matches_text(".other::part(label)", scene.label),
                "only the nearest host matches: the outer part is not the nested card's") &&
         ok;
    ok = expect(matches_text(".other::part(label)", scene.nested_label),
                "the nested part resolves its own host") &&
         ok;
    ok = expect(!matches_text(".card::part(label)", scene.nested_label),
                "a rule does not cross two component boundaries") &&
         ok;
    ok = expect(matches_text("::part(label)", scene.label), "the host selector may be omitted") &&
         ok;
    ok =
        expect(!matches_text("::part(label)", scene.host), "the host itself is not its own part") &&
        ok;
    ok = expect(!matches_text("::part(loose)", scene.loose),
                "a part outside a component subtree never matches") &&
         ok;
    ok = expect(!matches_text("::part(other)", scene.label), "the name must match") && ok;
    ok = expect(matches_text(".page Card::part(label)", scene.label),
                "an ancestor selector matches above the boundary") &&
         ok;
    ok = expect(!matches_text("Card::part(label) .text", scene.label),
                "a part may not be an ancestor compound") &&
         ok;

    // `part="tab active"`: every requested name must be present, in any order.
    scene.label.part = "tab active";
    ok = expect(matches_text("Card::part(tab active)", scene.label) &&
                    matches_text("Card::part(active tab)", scene.label),
                "multiple part names are order independent") &&
         ok;
    ok = expect(!matches_text("Card::part(tab missing)", scene.label),
                "every requested name must be present") &&
         ok;

    // `:hover` after `::part()` belongs to the part element, not to the host.
    scene.label.part = "label";
    ok = expect(!matches_text(".card::part(label):hover", scene.label),
                "the part-side pseudo-class reads the part state") &&
         ok;
    scene.label.state = tessera::ui::style::PseudoState::Hover;
    ok = expect(matches_text(".card::part(label):hover", scene.label),
                "a hovered part matches its own state") &&
         ok;
    scene.label.state = tessera::ui::style::PseudoState::None;
    scene.host.state = tessera::ui::style::PseudoState::Hover;
    ok = expect(!matches_text(".card::part(label):hover", scene.label),
                "a hovered host does not hover the part") &&
         ok;
    scene.host.state = tessera::ui::style::PseudoState::None;

    // The selector index must reach a part rule through the part name, not through the host tag.
    tessera::ui::style::SelectorIndex index;
    index.add(parse_selector_list("Card::part(label)").list, 7);
    const std::vector<SelectorMatch> matched = index.match(scene.label);
    ok = expect(matched.size() == 1 && matched.front().value == 7,
                "the index buckets a part subject by its part name") &&
         ok;
    ok = expect(index.match(scene.host).empty(), "the host is not a candidate") && ok;
    return ok;
}

} // namespace

// --- the scene path and the cascade -----------------------------------------------------------

namespace {

// root -> `<Card>` instance (component host, className `card`) -> View -> Text part="label".
struct ComponentScene {
    Node root{1};
    Node* host = nullptr;
    Node* label = nullptr;

    ComponentScene() {
        auto host_node = std::make_unique<Node>(2);
        host_node->set_element_kind("Component");
        host_node->set_component_name("Card");
        host_node->set_class_name("card");
        Node& host_ref = root.add_child(std::move(host_node));
        host = &host_ref;
        auto card_root = std::make_unique<Node>(3);
        card_root->set_element_kind("View");
        Node& card_root_ref = host->add_child(std::move(card_root));
        auto label_node = std::make_unique<Node>(4);
        label_node->set_element_kind("Text");
        label_node->set_part("label");
        Node& label_ref = card_root_ref.add_child(std::move(label_node));
        label = &label_ref;
    }
};

bool cascade() {
    ComponentScene scene;
    SceneStyleTree tree(scene.root);
    bool ok = true;

    const CompiledStyleSheet sheet = compile_stylesheet(R"(
        Card::part(label) { color: red; }
        .card::part(label):hover { color: green; }
    )");
    const ComputedStyle style = tree.compute(sheet, *scene.label, nullptr);
    const std::string* color = style.find("color");
    ok = expect(color != nullptr && *color == "red",
                "a `::part()` rule styles the part across the boundary") &&
         ok;

    scene.label->set_pseudo_state(tessera::ui::style::PseudoState::Hover);
    const ComputedStyle hovered = tree.compute(sheet, *scene.label, nullptr);
    const std::string* hover_color = hovered.find("color");
    ok = expect(hover_color != nullptr && *hover_color == "green",
                "`:hover` after `::part()` resolves the part's own state") &&
         ok;

    // Without a component boundary the same rule must not apply.
    Node plain_root(10);
    plain_root.set_element_kind("View");
    auto plain = std::make_unique<Node>(11);
    plain->set_element_kind("Text");
    plain->set_part("label");
    Node& plain_ref = plain_root.add_child(std::move(plain));
    SceneStyleTree plain_tree(plain_root);
    const ComputedStyle plain_style = plain_tree.compute(sheet, plain_ref, nullptr);
    const std::string* plain_color = plain_style.find("color");
    ok = expect(plain_color == nullptr || *plain_color != "red",
                "a part outside a component is not styled by `::part()`") &&
         ok;
    return ok;
}

// --- undeclared-part diagnostic and `@supports` -----------------------------------------------

bool diagnostics() {
    ComponentPartRegistry registry;
    const std::vector<std::string_view> declared = {"label", "value"};
    registry.declare("Field", declared);
    bool ok = true;

    const auto declared_only = check_part_selectors(
        tessera::ui::style::prepare_stylesheet("Field::part(label) {color:red}"), registry);
    ok = expect(declared_only.empty(), "a declared part is not diagnosed") && ok;

    const auto unknown = check_part_selectors(
        tessera::ui::style::prepare_stylesheet("Field::part(titel) {color:red}"), registry);
    ok = expect(unknown.size() == 1 && unknown.front().kind == StyleDiagnosticKind::UndeclaredPart,
                "an undeclared library part produces one diagnostic") &&
         ok;
    ok = expect(!unknown.empty() && unknown.front().message.find("titel") != std::string::npos &&
                    unknown.front().message.find("label") != std::string::npos,
                "the diagnostic names the part and the declared parts") &&
         ok;

    // Twice in one sheet: one diagnostic per (rule, part name), and the rule offsets differ.
    const auto twice =
        check_part_selectors(tessera::ui::style::prepare_stylesheet(
                                 "Field::part(titel) {color:red}\nField::part(titel) {color:blue}"),
                             registry);
    ok = expect(twice.size() == 1, "a repeated undeclared part is reported once per name") && ok;

    // An app component is not registered: nothing is diagnosed for it.
    const auto app = check_part_selectors(
        tessera::ui::style::prepare_stylesheet("Card::part(anything) {color:red}"), registry);
    ok = expect(app.empty(), "an app component's parts are not diagnosed") && ok;

    // One of two names is missing: exactly one diagnostic, for the missing one.
    const auto partial = check_part_selectors(
        tessera::ui::style::prepare_stylesheet("Field::part(label nope) {color:red}"), registry);
    ok = expect(partial.size() == 1 && partial.front().message.find("nope") != std::string::npos,
                "only the undeclared name of a multi-name part is diagnosed") &&
         ok;
    return ok;
}

bool supports_queries() {
    bool ok = true;
    ok = expect(supports("selector(.card::part(label))"), "@supports selector(::part) is true") &&
         ok;
    ok = expect(supports("selector(::part(label))"), "a bare `::part()` selector is supported") &&
         ok;
    ok = expect(!supports("selector(::slotted(x))"), "an unimplemented pseudo-element is false") &&
         ok;
    ok = expect(!supports("selector(::before)"),
                "a parse-only pseudo-element never matches, so it is not supported") &&
         ok;
    ok = expect(!supports("selector(::part(2x))"), "a malformed part selector is not supported") &&
         ok;
    ok = expect(supports("selector(.a) and (color: red)"),
                "selector() composes with declarations") &&
         ok;

    // A stylesheet can gate its `::part()` rules on support.
    const CompiledStyleSheet gated = compile_stylesheet(R"(
        @supports selector(::part(label)) { .part { color: red; } }
        @supports selector(::part(2x)) { .part { color: blue; } }
    )");
    StyleNode node;
    node.classes = {"part"};
    const ComputedStyle style = gated.compute(node, nullptr);
    const std::string* color = style.find("color");
    ok = expect(color != nullptr && *color == "red", "the supported block applies") && ok;
    return ok;
}

// --- compiled-stylesheet round trip ------------------------------------------------------------

bool blob_round_trip() {
    const std::string_view css = "Card::part(label active):hover { color: red; padding: 4px; }";
    const tessera::ui::style::PreparedStyleSheet prepared =
        tessera::ui::style::prepare_stylesheet(css);
    bool ok = true;
    ok = expect(prepared.items.size() == 1, "one rule is prepared") && ok;

    const std::vector<std::uint8_t> bytes = serialize_stylesheet(prepared);
    const auto loaded = load_stylesheet_blob(bytes);
    ok = expect(loaded.has_value(), "the blob loads") && ok;
    if (!loaded) {
        return false;
    }
    ok = expect(loaded->items.size() == 1, "the rule survives the round trip") && ok;
    if (loaded->items.empty()) {
        return false;
    }
    const SelectorList& selectors = loaded->items.front().selectors;
    ok =
        expect(selectors.selectors.size() == 1 && selectors.selectors.front().compounds.size() == 1,
               "the complex selector survives") &&
        ok;
    const auto& compound = selectors.selectors.front().compounds.front();
    ok = expect(compound.tag == "Card", "the host-side type survives") && ok;
    ok = expect(compound.pseudos.size() == 2 && compound.pseudos[0].name == "part" &&
                    compound.pseudos[0].pseudo_element &&
                    compound.pseudos[0].argument == "label active",
                "the part pseudo-element and its names survive") &&
         ok;
    ok = expect(compound.pseudos.size() == 2 && compound.pseudos[1].name == "hover",
                "the part-side pseudo-class survives") &&
         ok;

    // The reloaded chain must still match: the blob is the engine's own input, not a copy.
    const CompiledStyleSheet sheet(*loaded, Origin::Author);
    ComponentScene scene;
    // `part` and `class` are captured when the tree is built, like `className`; the pseudo-state is
    // refreshed per resolve.
    scene.label->set_part("label active");
    SceneStyleTree tree(scene.root);
    scene.label->set_pseudo_state(tessera::ui::style::PseudoState::Hover);
    const ComputedStyle style = tree.compute(sheet, *scene.label, nullptr);
    const std::string* color = style.find("color");
    ok = expect(color != nullptr && *color == "red",
                "a part rule loaded from the blob styles the part") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = parsing() && ok;
    ok = specificity() && ok;
    ok = matching() && ok;
    ok = cascade() && ok;
    ok = diagnostics() && ok;
    ok = supports_queries() && ok;
    ok = blob_round_trip() && ok;
    if (!ok) {
        std::cerr << "part_pseudo: FAILED\n";
        return 1;
    }
    std::cout << "part_pseudo: ok\n";
    return 0;
}
