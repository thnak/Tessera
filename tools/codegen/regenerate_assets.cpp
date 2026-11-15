// SPDX-License-Identifier: MIT

// The build-time asset embeds of the codegen CLI (issue #81, ADR-048): the compiled stylesheet
// blobs and the theme asset. Split out of regenerate.cpp so the driver stays within the file size
// rules and keeps to its own job; the two concerns are the only users of the stylesheet engine
// here.
//
// `StylesheetEmbed` and `EmbeddedTheme` are codegen's own plain-data records, so this unit sees the
// style engine but the codegen library still does not (the layering rule of AGENTS.md item 4).

#include "diagnostic_record.hpp"
#include "regenerate_assets.hpp"
#include "tessera/compiler/ir/node.hpp"
#include "tessera/compiler/lowering/library_components.hpp"
#include "tessera/ui/style/component_parts.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/stylesheet_blob.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {

using internal::read_file;
using internal::report;

namespace {

namespace fs = std::filesystem;
namespace ir = tessera::compiler::ir;
namespace lowering = tessera::compiler::lowering;
namespace style = tessera::ui::style;
// `path:line:column` for a byte offset into a stylesheet: the engine reports offsets, the CLI's
// contract with the user is file:line:column (docs/guides/developer-workflow.md).
std::string css_diagnostic_text(std::string_view path, std::string_view text, std::size_t offset,
                                std::string_view message) {
    const DiagnosticSpan span = css_span(path, text, offset);
    return std::string(path) + ":" + std::to_string(span.line) + ":" + std::to_string(span.column) +
           ": " + std::string(message);
}

// The stylesheet parser is tolerant by design (stylesheet-v1.md section 10), so a build-time
// failure can only come from the lexical error it proves: an unterminated string. Everything else
// prepares, and the prepared sheet carries its own diagnostics for the run time.
std::optional<std::string> stylesheet_syntax_error(std::string_view path, std::string_view text) {
    style::Tokenizer tokenizer(text);
    while (true) {
        const style::Token token = tokenizer.next();
        if (token.type == style::TokenType::Eof) {
            return std::nullopt;
        }
        if (token.type == style::TokenType::BadString) {
            return css_diagnostic_text(path, text, token.offset,
                                       "TESSERA_CSS_SYNTAX: unterminated string in the stylesheet");
        }
    }
}

// The parts a component source declares: every literal `part` attribute of its static node tree
// (stylesheet-v1 section 6.3). A pre-order walk, so the collected set is deterministic.
void collect_declared_parts(const ir::Node& node, std::set<std::string>& out) {
    const auto found = node.props.find("part");
    if (found != node.props.end() && found->second) {
        const ir::Expr& value = *found->second;
        if (value.kind == ir::ExprKind::Literal &&
            std::holds_alternative<std::string>(value.literal)) {
            const std::string& text = std::get<std::string>(value.literal);
            for (std::size_t index = 0;; ++index) {
                const std::string_view name = style::part_name_at(text, index);
                if (name.empty()) {
                    break;
                }
                out.emplace(name);
            }
        }
    }
    for (const ir::Node& child : node.children) {
        collect_declared_parts(child, out);
    }
    if (node.fallback) {
        collect_declared_parts(*node.fallback, out);
    }
    if (node.item_template) {
        collect_declared_parts(*node.item_template, out);
    }
}

// The part declarations of the library components: their part names are the only published styling
// contract, so they are the only ones `::part()` selectors are checked against (ADR-047 item 3).
// App components are absent by construction, which is why they are never diagnosed.
style::ComponentPartRegistry library_part_registry() {
    style::ComponentPartRegistry registry;
    for (const lowering::LibraryComponent& component : lowering::library_components()) {
        std::set<std::string> names;
        collect_declared_parts(lowering::make_library_component(component).root, names);
        std::vector<std::string_view> parts;
        parts.reserve(names.size());
        for (const std::string& name : names) {
            parts.push_back(name);
        }
        registry.declare(std::string(component.name), parts);
    }
    return registry;
}

} // namespace

// Compiled stylesheet blobs of the module (issue #81, ADR-048): the CLI compiles the CSS it read,
// so `StylesheetEmbed::bytes` is exactly `serialize_stylesheet()` output and the codegen library
// never sees a stylesheet engine header.
std::optional<std::vector<StylesheetEmbed>>
build_stylesheet_embeds(std::span<const std::string> paths, std::span<const std::string> texts,
                        Outcome& outcome) {
    std::vector<StylesheetEmbed> embeds;
    embeds.reserve(paths.size());
    const style::ComponentPartRegistry parts = library_part_registry();
    for (std::size_t i = 0; i < paths.size(); ++i) {
        if (const std::optional<std::string> error = stylesheet_syntax_error(paths[i], texts[i])) {
            report(outcome, true, *error);
            outcome.exit_code = 1;
            return std::nullopt;
        }
        const style::PreparedStyleSheet prepared = style::prepare_stylesheet(texts[i]);
        // `::part(name)` on a library component that does not declare `name` is a warning, not an
        // error: the rule simply never matches (stylesheet-v1 section 6.3).
        for (const style::StyleDiagnostic& diagnostic :
             style::check_part_selectors(prepared, parts)) {
            // The stylesheet engine reports byte offsets, so the record's span is derived once
            // here, from the same offset the printed text uses, and both stay in agreement.
            DiagnosticRecord record;
            record.code = "TESSERA_CSS_UNDECLARED_PART";
            record.severity = "warning";
            record.message = diagnostic.message;
            record.span = css_span(paths[i], texts[i], diagnostic.offset);
            report(outcome, false,
                   css_diagnostic_text(paths[i], texts[i], diagnostic.offset,
                                       "TESSERA_CSS_UNDECLARED_PART: " + diagnostic.message),
                   std::move(record));
        }
        embeds.push_back(StylesheetEmbed{paths[i], style::serialize_stylesheet(prepared)});
    }
    return embeds;
}

// The embedded theme asset (issue #81, ADR-048): a project without `assets.theme` has no asset and
// therefore no generated theme file. A configured asset is parsed and serialized once here, so the
// codegen library never sees the JSON or the style engine (docs/specs/stylesheet-v1.md 9.1).
bool build_theme_embed(const ProjectIdentity& identity, Outcome& outcome,
                       std::optional<EmbeddedTheme>& theme) {
    if (identity.theme.empty()) {
        return true;
    }
    const std::optional<std::string> text = read_file(identity.root / fs::path(identity.theme));
    if (!text) {
        report(outcome, true, identity.theme + ": TESSERA_THEME_READ: cannot read the theme asset");
        outcome.exit_code = 1;
        return false;
    }
    const auto parsed = style::parse_theme_asset(*text);
    if (!parsed) {
        report(outcome, true,
               identity.theme + ": " + style::theme_error_code_name(parsed.error().code) + ": " +
                   parsed.error().message);
        outcome.exit_code = 1;
        return false;
    }
    std::vector<style::ThemePin> pins;
    for (const ThemePinSpec& spec : identity.theme_pins) {
        pins.push_back(style::ThemePin{spec.theme, spec.pin});
    }
    if (const auto pin_error = style::check_theme_pins(*parsed, pins)) {
        report(outcome, true,
               identity.theme + ": " + style::theme_error_code_name(pin_error->code) + ": " +
                   pin_error->message);
        outcome.exit_code = 1;
        return false;
    }
    for (const std::string& warning : parsed->warnings) {
        report(outcome, false, identity.theme + ": warning: " + warning);
    }
    theme = EmbeddedTheme{style::serialize_theme_asset(*parsed)};
    return true;
}

} // namespace tessera::compiler::codegen::cli::internal