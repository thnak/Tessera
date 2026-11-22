// SPDX-License-Identifier: MIT
//
// The theme source build (spike #662, ADR-064 item 6): a role-token theme source compiles to plain
// CSS custom properties inside `@layer`, and one control (Button) renders through the engine from
// it in light, dark and high contrast.
//
// What this file proves, in the order the issue's evidence list asks for it:
//   1. the build output is plain CSS: one `@layer` block, one `:root` rule of custom properties,
//      and one rule per component key, with no runtime and no generated class names;
//   2. a colour role is one token whose value is a `light-dark()` pair, so there are no separate
//      light and dark names, and a scheme-independent token is emitted once (a `light-dark()` on a
//      length is CSS the engine rejects);
//   3. `extends` resolves into one flat theme, and a derived theme overrides per declaration;
//   4. the emitted CSS is CSS the engine actually accepts: it compiles through the real stylesheet
//      pipeline and the cascade resolves the tokens, in both schemes;
//   5. the vocabulary cost is reported: which tokens the components read, and which the theme
//      defines that nothing reads.

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/contrast.hpp"
#include "tessera/ui/style/scene_style.hpp"
#include "tessera/ui/style/theme_source.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;
namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "theme_source: " << message << "\n";
        ++failures;
    }
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// A role-token theme source in the shape ADR-064 item 6 describes: one token per role, a
// `light-dark()` pair for every colour, and component keys that read those tokens.
constexpr std::string_view kBaseTheme = R"json({
  "name": "spike-base",
  "tokens": {
    "--color-background-surface": ["#ffffff", "#101418"],
    "--color-background-raised": ["#f4f6f8", "#1a2027"],
    "--color-text-primary": ["#14171a", "#f2f4f6"],
    "--color-border-control": ["#6b747d", "#8b949e"],
    "--color-accent-fill": ["#0b5cad", "#4c9be8"],
    "--color-accent-ink": ["#ffffff", "#0b1220"],
    "--space-control-x": "12px",
    "--radius-control": "4px",
    "--font-control": "\"IBM Plex Sans\", sans-serif"
  },
  "components": {
    "Button": {
      "class": "tbtn",
      "keys": {
        "base": {
          "display": "flex",
          "padding": "0px var(--space-control-x)",
          "border-radius": "var(--radius-control)",
          "font-family": "var(--font-control)",
          "background-color": "var(--color-accent-fill)",
          "border-color": "var(--color-accent-fill)",
          "color": "var(--color-accent-ink)"
        },
        "variant:secondary": {
          "background-color": "var(--color-background-raised)",
          "border-color": "var(--color-border-control)",
          "color": "var(--color-text-primary)"
        },
        "state:hover": {
          "background-color": "var(--color-background-raised)"
        }
      }
    }
  }
})json";

// A derived theme: it restates one token and one declaration of one key, and inherits the rest.
constexpr std::string_view kDerivedTheme = R"json({
  "name": "spike-derived",
  "extends": "spike-base",
  "tokens": {
    "--color-accent-fill": ["#7a1f1f", "#e88a8a"]
  },
  "components": {
    "Button": {
      "class": "tbtn",
      "keys": {
        "base": {
          "border-radius": "2px"
        }
      }
    }
  }
})json";

style::ThemeSource parse_or_fail(std::string_view text, const char* what) {
    const auto parsed = style::parse_theme_source(text);
    if (!parsed) {
        check(false,
              std::string("parsing the ") + what + " source failed: " + parsed.error().message);
        return style::ThemeSource{};
    }
    return *parsed;
}

// The build of the base theme alone, which most cases below read.
style::ThemeBuild build_base() {
    const style::ThemeSource base = parse_or_fail(kBaseTheme, "base");
    const style::ThemeSource* chain[] = {&base};
    const auto built = style::build_theme_source(chain);
    if (!built) {
        check(false, "building the base theme failed: " + built.error().message);
        return style::ThemeBuild{};
    }
    return *built;
}

// 1. The output is plain CSS in one `@layer`, with one `:root` token rule and one rule per key.
void test_output_shape() {
    const style::ThemeBuild build = build_base();
    check(contains(build.css, "@layer tessera-ui.theme {"), "the build lands in one @layer block");
    check(contains(build.css, ":root {"), "the tokens are written under :root");
    check(contains(build.css, ".tbtn {"), "the base key compiles to the component class");
    check(contains(build.css, ".tbtn-secondary {"), "a variant key compiles to a class suffix");
    check(contains(build.css, ".tbtn:hover {"), "a state key compiles to a pseudo-class");
    // No generated class names: ADR-064 item 5 says a theme reaches a component through a stable
    // class, so the output must not invent one.
    check(!contains(build.css, "css-"), "the build invents no generated class name");
    // The block is balanced: one `@layer` opening brace and one closing brace at the end.
    check(build.css.ends_with("}\n"), "the layer block is closed");
}

// 2. One token per role, `light-dark()` for a colour and a bare value for a length.
void test_role_tokens() {
    const style::ThemeBuild build = build_base();
    check(contains(build.css, "--color-background-surface: light-dark(#ffffff, #101418);"),
          "a colour role is one token holding a light-dark() pair");
    check(contains(build.css, "--color-text-primary: light-dark(#14171a, #f2f4f6);"),
          "the text role is a light-dark() pair too");
    // A length is not a colour: emitting `light-dark()` for it would be CSS the engine rejects.
    check(contains(build.css, "--space-control-x: 12px;"),
          "a scheme-independent token is emitted once, not as a light-dark() pair");
    check(!contains(build.css, "--space-control-x: light-dark("),
          "a length never takes a light-dark() pair");
    // There are no separate light and dark names, which is the property ADR-064 item 6 protects.
    check(!contains(build.css, "--color-background-surface-light"),
          "there is no separate light token name");
    check(!contains(build.css, "--color-background-surface-dark"),
          "there is no separate dark token name");
}

// 3. `extends` resolves into one flat theme, and a derived theme overrides per declaration.
void test_extends() {
    const style::ThemeSource base = parse_or_fail(kBaseTheme, "base");
    const style::ThemeSource derived = parse_or_fail(kDerivedTheme, "derived");
    const style::ThemeSource* chain[] = {&base, &derived};
    const auto built = style::build_theme_source(chain);
    if (!built) {
        check(false, "building the derived chain failed: " + built.error().message);
        return;
    }
    // The derived token replaces the base one, and the base tokens it does not restate survive.
    check(contains(built->css, "--color-accent-fill: light-dark(#7a1f1f, #e88a8a);"),
          "a derived theme overrides a token");
    check(contains(built->css, "--color-background-surface: light-dark(#ffffff, #101418);"),
          "a token the derived theme does not restate survives the merge");
    // The derived declaration replaces the base one, and the base declarations survive.
    check(contains(built->css, "border-radius: 2px;"), "a derived theme overrides one declaration");
    check(contains(built->css, "padding: 0px var(--space-control-x);"),
          "a declaration the derived theme does not restate survives the merge");
    // A chain that does not link up is an error, not a silent partial merge.
    const style::ThemeSource* broken[] = {&derived};
    const auto failed = style::build_theme_source(broken);
    check(!failed && failed.error().code == style::ThemeSourceErrorCode::Extends,
          "a chain whose first source extends something absent is an error");
}

// 4. The emitted CSS is CSS the engine accepts, and the cascade resolves it in both schemes.
void test_engine_accepts_the_output() {
    const style::ThemeBuild build = build_base();
    const auto prepared = style::prepare_stylesheet(build.css);
    check(prepared.items.size() >= 4,
          "the engine's parser accepts the built CSS and finds the token and component rules");

    // The cascade resolves the tokens: a Button node with the component class gets the accent fill
    // in the light scheme and the dark fill in the dark scheme, from the same token name.
    ui::Node root(1);
    root.set_class_name("root");
    auto button = std::make_unique<ui::Node>(2);
    button->set_class_name("tbtn");
    ui::Node& button_ref = root.add_child(std::move(button));
    ui::Node* button_ptr = &button_ref;

    style::SceneStyleTree tree(root);
    style::CompiledStyleSheet light(prepared, style::Origin::Author);
    const style::ComputedStyle light_style = tree.compute(light, *button_ptr);
    const std::string* light_fill = light_style.find("background-color");
    check(light_fill != nullptr, "the base key sets a background colour");
    if (light_fill != nullptr) {
        check(*light_fill == "#0b5cad" || *light_fill == "rgb(11, 92, 173)",
              "the light scheme resolves the accent fill through the token, got `" + *light_fill +
                  "`");
    }

    // The same sheet plus one rule putting the root in the dark scheme: `light-dark()` picks the
    // other branch, so the token name did not change and the value did. The scheme is the element's
    // own `color-scheme` (inherited), which is what `var_resolve.cpp` reads.
    const auto dark_prepared =
        style::prepare_stylesheet(std::string(build.css) + ".root { color-scheme: dark; }\n");
    style::SceneStyleTree dark_tree(root);
    style::CompiledStyleSheet dark(dark_prepared, style::Origin::Author);
    const style::ComputedStyle dark_style = dark_tree.compute(dark, *button_ptr);
    const std::string* dark_fill = dark_style.find("background-color");
    check(dark_fill != nullptr, "the base key sets a background colour in the dark scheme too");
    if (dark_fill != nullptr && light_fill != nullptr) {
        check(*dark_fill != *light_fill,
              "the dark scheme resolves the same token to a different value, got `" + *dark_fill +
                  "` and `" + *light_fill + "`");
    }
}

// 5. The vocabulary cost: which tokens the components read, and which nothing reads.
void test_vocabulary_cost() {
    const style::ThemeBuild build = build_base();
    // The Button keys read the accent pair, the raised surface, the control border, the text role,
    // the control spacing, the control radius and the control font: eight of the nine tokens.
    check(build.read_tokens.size() == 8,
          "the Button keys read eight tokens, got " + std::to_string(build.read_tokens.size()));
    const auto reads = [&build](std::string_view name) {
        for (const std::string& token : build.read_tokens) {
            if (token == name) {
                return true;
            }
        }
        return false;
    };
    check(reads("--color-accent-fill"), "the accent fill is read");
    check(reads("--space-control-x"), "the control spacing is read");
    check(!reads("--color-background-surface"), "the page surface is not read by Button");
    // One token is defined and unread, and it is reported: that is the cost the spike measures.
    check(build.warnings.size() == 1,
          "one defined token is unread, got " + std::to_string(build.warnings.size()));
    for (const std::string& warning : build.warnings) {
        check(contains(warning, "TESSERA_THEME_SOURCE_UNUSED_TOKEN"),
              "an unread token is reported as TESSERA_THEME_SOURCE_UNUSED_TOKEN");
    }
}

// 6. A malformed source is a typed error naming the member, never a silent drop.
void test_parse_errors() {
    const auto bad_token = style::parse_theme_source(R"json({
      "name": "bad", "tokens": { "color-accent": "#fff" }
    })json");
    check(!bad_token && bad_token.error().code == style::ThemeSourceErrorCode::Token,
          "a token name without `--` is TESSERA_THEME_SOURCE_TOKEN");

    const auto bad_key = style::parse_theme_source(R"json({
      "name": "bad",
      "components": { "Button": { "class": "tbtn", "keys": { "colour:red": {} } } }
    })json");
    check(!bad_key && bad_key.error().code == style::ThemeSourceErrorCode::Key,
          "an unknown key form is TESSERA_THEME_SOURCE_KEY");

    const auto bad_pair = style::parse_theme_source(R"json({
      "name": "bad", "tokens": { "--a": ["#fff"] }
    })json");
    check(!bad_pair && bad_pair.error().code == style::ThemeSourceErrorCode::Shape,
          "a one-element light-dark() pair is TESSERA_THEME_SOURCE_SHAPE");

    const auto no_name = style::parse_theme_source(R"json({ "tokens": {} })json");
    check(!no_name && no_name.error().code == style::ThemeSourceErrorCode::Name,
          "a source without a name is TESSERA_THEME_SOURCE_NAME");

    // `adaptations` is accepted and ignored: the spike records it as a gap rather than rejecting a
    // source written against the full model.
    const auto with_adaptations = style::parse_theme_source(R"json({
      "name": "ok", "adaptations": [ { "when": "width < 600px" } ]
    })json");
    check(with_adaptations.has_value(),
          "`adaptations` parses, so a full-model source is not rejected");
}

// 7. The key-to-selector mapping is the documented one.
void test_key_selectors() {
    const auto base = style::theme_key_selector("tbtn", "base");
    check(base && *base == ".tbtn", "`base` is the component class");
    const auto variant = style::theme_key_selector("tbtn", "variant:primary");
    check(variant && *variant == ".tbtn-primary", "`variant:x` is a class suffix");
    const auto combined = style::theme_key_selector("tbtn", "variant:primary+size:sm");
    check(combined && *combined == ".tbtn-primary.tbtn-sm",
          "a combined key is the joined classes, got `" + (combined ? *combined : "") + "`");
    const auto state = style::theme_key_selector("tbtn", "variant:primary+state:hover");
    check(state && *state == ".tbtn-primary:hover", "a state key is a pseudo-class");
    check(!style::theme_key_selector("tbtn", "colour:red"), "an unknown key form has no selector");
}

// 8. The shipped fixture theme builds, and Button resolves through the engine in three modes.
//
// This is the issue's "render one control (Button) through the engine from it in light, dark and
// high contrast" evidence, in the form a headless test can assert: the same built CSS, compiled by
// the real stylesheet pipeline, resolves the Button's fill and ink in each mode, and the contrast
// the theme promises is measured rather than assumed.
void test_fixture_three_modes() {
    const std::string path = std::string(TESSERA_THEME_SOURCE_FIXTURE);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        check(false, "cannot read the fixture theme source at " + path);
        return;
    }
    std::ostringstream text;
    text << file.rdbuf();
    const style::ThemeSource source = parse_or_fail(text.str(), "fixture");
    const style::ThemeSource* chain[] = {&source};
    const auto built = style::build_theme_source(chain);
    if (!built) {
        check(false, "building the fixture theme failed: " + built.error().message);
        return;
    }

    // The fixture is a full role-token theme: every colour role is a light-dark() pair, and the
    // scheme-independent tokens are emitted once.
    check(contains(built->css, "--color-accent-fill: light-dark(#0b5cad, #4c9be8);"),
          "the fixture's accent role is one light-dark() pair");
    check(contains(built->css, "--control-height: 32px;"),
          "the fixture's control height is emitted once");

    const auto prepared = style::prepare_stylesheet(built->css);
    check(!prepared.items.empty(), "the engine accepts the fixture's built CSS");

    // Three modes: light, dark, and high contrast. High contrast is the same token set with the
    // stricter floors, so the spike measures it rather than emitting a fourth theme: ADR-064 item 6
    // says a role token is one name, and a high-contrast variant is a different *value* for the
    // same name, which is what `prefers-contrast` selects.
    struct Mode {
        const char* name;
        const char* scheme;
        bool high_contrast;
    };
    const Mode modes[] = {
        {"light", "light", false}, {"dark", "dark", false}, {"high-contrast", "light", true}};

    for (const Mode& mode : modes) {
        ui::Node root(1);
        root.set_class_name("root");
        auto button = std::make_unique<ui::Node>(2);
        button->set_class_name("tbtn tbtn-secondary");
        ui::Node& button_ref = root.add_child(std::move(button));

        const auto mode_prepared = style::prepare_stylesheet(
            std::string(built->css) + ".root { color-scheme: " + mode.scheme + "; }\n");
        style::SceneStyleTree tree(root);
        style::CompiledStyleSheet sheet(mode_prepared, style::Origin::Author);
        const style::ComputedStyle computed = tree.compute(sheet, button_ref);

        const std::string* fill = computed.find("background-color");
        const std::string* ink = computed.find("color");
        check(fill != nullptr, std::string(mode.name) + ": the secondary Button has a fill");
        check(ink != nullptr, std::string(mode.name) + ": the secondary Button has an ink");
        if (fill == nullptr || ink == nullptr) {
            continue;
        }
        // The fill and the ink are resolved colours, not token references: the cascade substituted
        // the custom property and picked the light-dark() branch.
        check(!contains(*fill, "var("), std::string(mode.name) + ": the fill is resolved");
        check(!contains(*ink, "var("), std::string(mode.name) + ": the ink is resolved");

        const auto fill_colour = style::parse_color(*fill);
        const auto ink_colour = style::parse_color(*ink);
        check(fill_colour.ok, std::string(mode.name) + ": the fill parses as a colour");
        check(ink_colour.ok, std::string(mode.name) + ": the ink parses as a colour");
        if (!fill_colour.ok || !ink_colour.ok) {
            continue;
        }
        // The measured contrast the issue asks for: the control's text clears the normal-text
        // floor. The secondary variant's fill is the raised surface, which is a subtle panel and
        // not the control's boundary, so the 3:1 non-text floor applies to
        // `--color-border-control` instead; the spike records that distinction rather than
        // asserting a floor the variant does not claim.
        const double text_ratio = style::contrast_ratio(ink_colour.color, fill_colour.color);
        const double text_floor =
            style::contrast_floor(style::ContrastRequirement::NormalText, mode.high_contrast);
        check(text_ratio >= text_floor, std::string(mode.name) +
                                            ": the Button ink on its fill is " +
                                            std::to_string(text_ratio) + ":1, below the " +
                                            std::to_string(text_floor) + ":1 floor");
    }

    // The vocabulary cost of the fixture: how many tokens it defines against how many Button reads.
    check(built->read_tokens.size() >= 10,
          "the fixture's Button keys read at least ten tokens, got " +
              std::to_string(built->read_tokens.size()));
    check(built->read_tokens.size() < source.tokens.size(),
          "the fixture defines more tokens than Button reads, which is the cost the spike records");
}

} // namespace

int main() {
    test_output_shape();
    test_role_tokens();
    test_extends();
    test_engine_accepts_the_output();
    test_vocabulary_cost();
    test_parse_errors();
    test_key_selectors();
    test_fixture_three_modes();
    if (failures != 0) {
        std::cerr << "theme_source: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "theme_source: ok\n";
    return 0;
}
