// SPDX-License-Identifier: MIT

// Headless accessibility-contract test of the library display components (issue #240, ADR-047 item
// 10), through the generated `DisplayPage`: an Avatar is a named `img`, a Spinner is a named
// indeterminate `progressbar`, a Divider and a Skeleton are hidden `presentation` boxes, and
// nothing on the page is focusable (these components are not controls). Class parts are checked
// too, since the stylesheet keys on them. No GPU, no clock.

#include "DisplayPage.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string aria(const ui::Node& node, std::string_view name) {
    for (const ui::AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == name) {
            return std::string(attribute.value);
        }
    }
    return "<absent>";
}

// Whether `name` is one of the space-separated classes of the node.
bool has_class(const ui::Node& node, std::string_view name) {
    std::string_view rest = node.class_name();
    while (!rest.empty()) {
        const std::size_t end = rest.find(' ');
        const std::string_view token = rest.substr(0, end);
        if (token == name) {
            return true;
        }
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    }
    return false;
}

void walk(ui::Node& node, std::vector<ui::Node*>& out) {
    out.push_back(&node);
    for (const auto& child : node.children()) {
        walk(*child, out);
    }
}

std::size_t count_class(const std::vector<ui::Node*>& nodes, std::string_view name) {
    return static_cast<std::size_t>(std::count_if(
        nodes.begin(), nodes.end(), [&](const ui::Node* node) { return has_class(*node, name); }));
}

} // namespace

int main() {
    ui::Node root{1000};
    ui::UpdateScheduler scheduler;
    gallery::DisplayPage page;
    page.build(root);
    page.attach_scheduler(scheduler);
    page.apply_props();
    scheduler.flush();
    std::vector<ui::Node*> nodes;
    walk(root, nodes);

    for (const ui::Node* node : nodes) {
        check(!node->focusable(), "no display component is focusable");
    }

    std::size_t avatars = 0;
    std::size_t spinners = 0;
    for (const ui::Node* node : nodes) {
        if (has_class(*node, "tav")) {
            ++avatars;
            check(node->role() == "img", "an Avatar is an img");
            check(aria(*node, "aria-label") != "<absent>" && aria(*node, "aria-label") != "",
                  "an Avatar is named");
        }
        if (has_class(*node, "tspn")) {
            ++spinners;
            check(node->role() == "progressbar", "a Spinner is a progressbar");
            check(aria(*node, "aria-label") != "<absent>", "a Spinner is named");
            check(aria(*node, "aria-valuenow") == "<absent>", "a Spinner is indeterminate");
        }
        if (has_class(*node, "tdiv") || has_class(*node, "tsk")) {
            check(node->role() == "presentation" && aria(*node, "aria-hidden") == "true",
                  "a Divider or Skeleton is hidden from the accessibility tree");
        }
    }
    check(avatars == 3, "three Avatars: " + std::to_string(avatars));
    check(spinners == 2, "two Spinners: " + std::to_string(spinners));
    check(count_class(nodes, "tdiv-vertical") == 1 && count_class(nodes, "tdiv-horizontal") == 1,
          "one vertical and one horizontal Divider");
    check(count_class(nodes, "tbdg") == 6, "six Badges");
    check(count_class(nodes, "tbdg-danger") == 1, "the tone is a class");
    check(count_class(nodes, "tcard-flat") == 2 && count_class(nodes, "tcard-raised") == 1,
          "Card variants are classes");
    check(count_class(nodes, "tcard-title") == 2,
          "a titled Card draws its title, an untitled none");
    check(count_class(nodes, "tsk-circle") == 1 && count_class(nodes, "tsk-text") == 2 &&
              count_class(nodes, "tsk-rect") == 1,
          "Skeleton shapes are classes");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "display_runtime: ok\n";
    return 0;
}
