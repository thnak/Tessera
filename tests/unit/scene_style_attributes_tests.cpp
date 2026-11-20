// SPDX-License-Identifier: MIT

// The style tree exposes a node's `role` and `aria-*` pairs as attributes, so a selector can name
// them as in a browser (`[role="list"]`, `[aria-level="2"]`). The user-agent sheet relies on it to
// tell `h1` from `h6` (issue #606).

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string_view>

namespace {

namespace style = tessera::ui::style;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_style_attributes: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    bool ok = true;

    Node root(1);
    root.set_element_kind("View");
    auto heading = std::make_unique<Node>(2);
    heading->set_element_kind("Text");
    heading->set_role("heading");
    heading->set_aria("aria-level", "2");
    Node* heading_ptr = heading.get();
    auto plain = std::make_unique<Node>(3);
    plain->set_element_kind("Text");
    Node* plain_ptr = plain.get();
    root.add_child(std::move(heading));
    root.add_child(std::move(plain));

    style::SceneStyleTree tree(root);
    const style::StyleNode* view = tree.view(*heading_ptr);
    ok &= expect(view != nullptr, "the heading has a style view");
    if (view != nullptr) {
        bool role = false;
        bool level = false;
        for (const style::AttributeView& attribute : view->attributes) {
            role |= attribute.name == "role" && attribute.value == "heading";
            level |= attribute.name == "aria-level" && attribute.value == "2";
        }
        ok &= expect(role, "role is an attribute");
        ok &= expect(level, "aria-level is an attribute");
    }
    const style::StyleNode* none = tree.view(*plain_ptr);
    ok &= expect(none != nullptr && none->attributes.empty(),
                 "a node with no role or aria has no attributes");

    // A selector on the attribute matches the heading and not the plain text.
    const style::CompiledStyleSheet sheet = style::compile_stylesheet(
        "Text[aria-level=\"2\"] { font-size: 24px; }\n[role=\"heading\"] { font-weight: 700; }\n");
    const style::ComputedStyle matched = sheet.compute(*tree.view(*heading_ptr), nullptr);
    ok &= expect(matched.find("font-size") != nullptr && *matched.find("font-size") == "24px",
                 "[aria-level] selects the heading");
    ok &= expect(matched.find("font-weight") != nullptr && *matched.find("font-weight") == "700",
                 "[role] selects the heading");
    const style::ComputedStyle unmatched = sheet.compute(*tree.view(*plain_ptr), nullptr);
    ok &= expect(unmatched.find("font-size") == nullptr || *unmatched.find("font-size") != "24px",
                 "plain text is not matched");

    return ok ? 0 : 1;
}
