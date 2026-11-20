// SPDX-License-Identifier: MIT

// The user-agent stylesheet (issue #602, ADR-063): an unstyled control behaves the way a browser's
// does, and any author declaration beats it regardless of specificity or order.

#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/ua_stylesheet.hpp"

#include <array>
#include <iostream>
#include <span>
#include <string_view>

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
style::ComputedStyle compute_for(std::string_view tag, std::string_view author) {
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
    ok &= value_is(button, "padding-top", "1px");
    ok &= value_is(button, "padding-left", "6px");
    // The padding must not grow an authored width, as in a browser's button (border-box).
    ok &= value_is(button, "box-sizing", "border-box");

    // The sheet is a list of elements, not a global reset: a View keeps the engine's own defaults.
    const style::ComputedStyle view = compute_for("View", "");
    ok &= value_is(view, "align-items", "stretch");
    ok &= value_is(view, "text-align", "start");
    ok &= value_is(view, "padding-top", "0");

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
