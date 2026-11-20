// SPDX-License-Identifier: MIT

// The user-agent stylesheet (issue #602, ADR-063): an unstyled control behaves the way a browser's
// does, and any author declaration beats it regardless of specificity or order.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/ua_stylesheet.hpp"

#include <array>
#include <iostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace style = tessera::ui::style;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "ua_stylesheet: " << message << "\n";
    }
    return condition;
}

bool value_is(const style::ComputedStyle& computed, const char* property,
              std::string_view expected) {
    const std::string* value = computed.find(property);
    if (value == nullptr || *value != expected) {
        std::cerr << "ua_stylesheet: " << property << " is "
                  << (value == nullptr ? std::string("<unset>") : *value) << ", expected "
                  << expected << "\n";
        return false;
    }
    return true;
}

// UA first, then the author sheet (if any), the way the preview and the app host build their
// cascade.
style::ComputedStyle compute_for(std::string_view tag, std::string_view author,
                                 std::vector<style::AttributeView> attributes = {},
                                 bool component_host = false) {
    const style::PreparedStyleSheet authored = style::prepare_stylesheet(author);
    std::array<style::StylesheetSource, 2> sources{
        style::StylesheetSource{"user-agent", &style::ua_prepared_stylesheet(),
                                style::Origin::UserAgent, false},
        style::StylesheetSource{"author", &authored, style::Origin::Author, false}};
    const std::span<const style::StylesheetSource> used(sources.data(), author.empty() ? 1 : 2);
    const style::CompiledStyleSheet sheet(used);
    style::StyleNode root;
    root.tag = "Root";
    style::StyleNode node;
    node.tag = tag; // a view: the callers pass string literals
    node.component_host = component_host;
    node.attributes = std::move(attributes);
    node.parent = &root;
    root.children.push_back(&node);
    return sheet.compute(node, nullptr);
}

} // namespace

int main() {
    bool ok = true;

    ok &= expect(
        style::ua_prepared_stylesheet().diagnostics.empty(),
        "the user-agent sheet has diagnostics: every rule must be one the engine understands");

    // A bare Button centres its label on both axes, like a browser's <button> (issue #602).
    const style::ComputedStyle button = compute_for("Button", "");
    ok &= value_is(button, "display", "flex");
    ok &= value_is(button, "flex-direction", "row");
    ok &= value_is(button, "align-items", "center");
    ok &= value_is(button, "justify-content", "center");
    ok &= value_is(button, "text-align", "center");
    ok &= value_is(button, "padding-top", "2px");
    ok &= value_is(button, "padding-left", "7px");
    // The padding must not grow an authored width, as in a browser's button (border-box).
    ok &= value_is(button, "box-sizing", "border-box");

    // A browser's div is `display: block`; the engine's own default is a flex row (issue #606).
    const style::ComputedStyle view = compute_for("View", "");
    ok &= value_is(view, "display", "block");
    ok &= value_is(view, "text-align", "start");
    ok &= value_is(view, "padding-top", "0");

    // Headings: bold, Chrome's size and margin per `aria-level` (issue #606).
    const style::ComputedStyle h1 =
        compute_for("Text", "", {{"role", "heading"}, {"aria-level", "1"}});
    ok &= value_is(h1, "font-weight", "700");
    ok &= value_is(h1, "font-size", "32px");
    ok &= value_is(h1, "margin-top", "0.67em");
    const style::ComputedStyle h6 =
        compute_for("Text", "", {{"role", "heading"}, {"aria-level", "6"}});
    ok &= value_is(h6, "font-size", "10.72px");
    // Plain text and an unleveled Text keep the engine defaults.
    const style::ComputedStyle text = compute_for("Text", "");
    ok &= expect(text.find("font-weight") == nullptr || *text.find("font-weight") != "700",
                 "a plain Text must not be bold");
    ok &= expect(text.find("margin-top") == nullptr || *text.find("margin-top") == "0",
                 "a plain Text must have no margin");

    // A component that is named like a built-in is the author's element: no UA default on its host.
    const style::ComputedStyle host = compute_for("Button", "", {}, true);
    ok &= expect(host.find("background-color") == nullptr ||
                     *host.find("background-color") != "#efefef",
                 "the user-agent sheet must not style a component host named Button");

    // Controls do not inherit `line-height` (a browser resets it to `normal`).
    ok &= value_is(compute_for("Button", ""), "line-height", "normal");
    ok &= value_is(compute_for("TextInput", ""), "line-height", "normal");

    // A paragraph has Chrome's 1em margin; a span (plain Text) does not (issue #606).
    const style::ComputedStyle paragraph = compute_for("Text", "", {{"role", "paragraph"}});
    ok &= value_is(paragraph, "margin-top", "1em");

    // A link is blue and underlined; a list has Chrome's margin and start padding.
    const style::ComputedStyle link = compute_for("Text", "", {{"role", "link"}});
    ok &= value_is(link, "color", "#0000ee");
    ok &= value_is(link, "text-decoration-line", "underline");
    const style::ComputedStyle list = compute_for("View", "", {{"role", "list"}});
    ok &= value_is(list, "padding-left", "40px");
    ok &= value_is(list, "margin-top", "1em");

    // Controls have a size, a border and the browser's control font; without it a bare TextInput
    // is zero wide (the audit's finding).
    const style::ComputedStyle input = compute_for("TextInput", "");
    ok &= value_is(input, "min-width", "176px");
    ok &= value_is(input, "min-height", "22px");
    ok &= value_is(input, "border-top-width", "1px");
    ok &= value_is(input, "font-size", "13.3333px");
    ok &= value_is(compute_for("TextArea", ""), "min-width", "161px");

    // Every author declaration wins over the user-agent one, with the weakest possible selector,
    // and the user-agent defaults the author did not touch stay.
    const style::ComputedStyle overridden =
        compute_for("Button", "Button { display: block; align-items: flex-start; padding: 0px; }");
    ok &= value_is(overridden, "display", "block");
    ok &= value_is(overridden, "align-items", "flex-start");
    ok &= value_is(overridden, "padding-top", "0px");
    ok &= value_is(overridden, "justify-content", "center");
    ok &= value_is(overridden, "text-align", "center");

    return ok ? 0 : 1;
}
