// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::ContainerAxis;
using tessera::ui::style::ContainerOp;
using tessera::ui::style::ContainerSize;
using tessera::ui::style::evaluate;
using tessera::ui::style::parse_container_query;
using tessera::ui::style::SceneStyleTree;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "container_query: " << message << "\n";
    }
    return condition;
}

bool holds(std::string_view prelude, double width, double height, bool block = true) {
    const auto parsed = parse_container_query(prelude);
    return parsed.ok && evaluate(parsed.query, ContainerSize{width, height, block});
}

struct Sizes {
    const Node* container = nullptr;
    ContainerSize size;
    bool has_size = true;
};

bool lookup(void* context, const Node& node, ContainerSize& out) {
    const auto* sizes = static_cast<const Sizes*>(context);
    if (&node != sizes->container || !sizes->has_size) {
        return false;
    }
    out = sizes->size;
    return true;
}

bool has_color(const ComputedStyle& style, std::string_view expected) {
    const std::string* value = style.find("background-color");
    return value != nullptr && *value == expected;
}

int parse_and_evaluate() {
    const auto named = parse_container_query("sidebar (min-width: 400px) and (max-width: 800px)");
    if (!expect(named.ok && named.query.name == "sidebar" && named.query.conditions.size() == 2,
                "named query with two conditions") ||
        !expect(named.query.conditions[0].op == ContainerOp::GreaterEqual &&
                    named.query.conditions[0].axis == ContainerAxis::Width &&
                    named.query.conditions[0].value == 400.0,
                "min-width is >=") ||
        !expect(named.query.conditions[1].op == ContainerOp::LessEqual, "max-width is <=")) {
        return 1;
    }
    if (!expect(holds("(min-width: 400px)", 400, 0), "min-width boundary holds") ||
        !expect(!holds("(min-width: 400px)", 399, 0), "min-width below fails") ||
        !expect(holds("(width > 400px)", 401, 0), "range > holds") ||
        !expect(!holds("(width > 400px)", 400, 0), "range > boundary fails") ||
        !expect(holds("(width <= 25rem)", 400, 0), "rem is 16px") ||
        !expect(holds("(inline-size >= 400px) and (height < 100px)", 400, 99),
                "and of two features") ||
        !expect(!holds("(inline-size >= 400px) and (height < 100px)", 400, 100),
                "and fails when one fails") ||
        !expect(holds("(block-size = 50px)", 0, 50), "block-size equals") ||
        !expect(!holds("(height >= 0)", 10, 10, false), "height is not queryable on inline-size")) {
        return 2;
    }
    for (const char* bad :
         {"", "(min-width: 10)", "(color: 1)", "not (width > 1px)",
          "(width > 1px) or (height > 1px)", "style(--x: 1)", "(width >", "(width > 1px) and"}) {
        if (!expect(!parse_container_query(bad).ok, bad)) {
            return 3;
        }
    }
    return 0;
}

int stylesheet_rules() {
    const auto sheet =
        compile_stylesheet("* { container-type: inline-size }"
                           "@container (min-width: 600px) { * { background-color: red } }"
                           "@container (width < 300px) { * { background-color: blue } }");
    if (!expect(sheet.has_container_queries(), "has container queries") ||
        !expect(sheet.diagnostics().empty(), "no diagnostics")) {
        return 4;
    }
    const auto bad = compile_stylesheet("@container (min-width: 10) { * { color: red } }");
    if (!expect(bad.diagnostics().size() == 1, "invalid container query is diagnosed") ||
        !expect(!bad.has_container_queries(), "invalid query adds no rules")) {
        return 5;
    }

    auto root = std::make_unique<Node>(1);
    Node* child = &root->add_child(std::make_unique<Node>(2));
    SceneStyleTree tree(*root);
    Sizes sizes{root.get(), ContainerSize{700, 100, false}};

    if (!expect(has_color(tree.compute(sheet, *child, lookup, &sizes), "red"),
                "wide container applies the rule") ||
        !expect(has_color(tree.compute(sheet, *root, lookup, &sizes), "transparent"),
                "a container is not styled by its own query")) {
        return 6;
    }
    sizes.size.width = 250;
    if (!expect(has_color(tree.compute(sheet, *child, lookup, &sizes), "blue"),
                "narrow container switches the rule")) {
        return 7;
    }
    sizes.size.width = 450;
    if (!expect(has_color(tree.compute(sheet, *child, lookup, &sizes), "transparent"),
                "mid width matches neither rule")) {
        return 8;
    }
    sizes.has_size = false;
    if (!expect(has_color(tree.compute(sheet, *child, lookup, &sizes), "transparent"),
                "a container without a size leaves its rules unapplied") ||
        !expect(has_color(tree.compute(sheet, *child), "transparent"),
                "no size source: container rules never apply")) {
        return 9;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = parse_and_evaluate(); rc != 0) {
        return rc;
    }
    if (const int rc = stylesheet_rules(); rc != 0) {
        return rc;
    }
    std::cout << "container_query: ok\n";
    return 0;
}
