// SPDX-License-Identifier: MIT

// Scene node element metadata (docs/design/scene-graph.md section 5): get/set/clear, dirty-style
// coupling, no-op-on-unchanged, borrowed view length caps and the accessibility pair cap. No GPU.

#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

std::string long_text(std::size_t size) {
    return std::string(size, 'x');
}

void test_element_kind() {
    ui::Node node(1);
    check(node.element_kind().empty(), "a fresh node has no element kind");
    node.set_element_kind("View");
    check_eq(node.element_kind(), "View", "element kind is read back");
    node.set_element_kind(long_text(ui::Node::kMaxMetadataBytes + 4));
    check(node.element_kind().size() == ui::Node::kMaxMetadataBytes,
          "element kind is truncated to the byte cap");
    check(node.dirty() == ui::Dirty::None, "element kind does not invalidate style or layout");
}

void test_id_and_class_dirty() {
    ui::Node node(1);
    node.clear_dirty(ui::Dirty::All);
    node.set_element_id("root");
    check_eq(node.element_id(), "root", "id is read back");
    check(ui::has_flag(node.dirty(), ui::Dirty::Style), "setting an id marks Dirty::Style");

    node.clear_dirty(ui::Dirty::All);
    node.set_element_id("root");
    check(node.dirty() == ui::Dirty::None, "setting the same id is a no-op");

    node.set_element_id("other");
    check(ui::has_flag(node.dirty(), ui::Dirty::Style), "a new id marks Dirty::Style");
    node.clear_dirty(ui::Dirty::All);
    node.clear_element_id();
    check(node.element_id().empty() && ui::has_flag(node.dirty(), ui::Dirty::Style),
          "clearing an id works and marks Dirty::Style");
    node.clear_dirty(ui::Dirty::All);
    node.clear_element_id();
    check(node.dirty() == ui::Dirty::None, "clearing an empty id is a no-op");

    node.set_class_name("box wide");
    check_eq(node.class_name(), "box wide", "className is read back");
    check(ui::has_flag(node.dirty(), ui::Dirty::Style), "setting className marks Dirty::Style");
    node.clear_dirty(ui::Dirty::All);
    node.set_class_name("box wide");
    check(node.dirty() == ui::Dirty::None, "setting the same className is a no-op");
    node.set_class_name(long_text(ui::Node::kMaxMetadataBytes + 4));
    check(node.class_name().size() == ui::Node::kMaxMetadataBytes,
          "className is truncated to the byte cap");
    node.clear_dirty(ui::Dirty::All);
    node.clear_class_name();
    check(node.class_name().empty() && ui::has_flag(node.dirty(), ui::Dirty::Style),
          "clearing className works and marks Dirty::Style");
}

void test_automation_and_role() {
    ui::Node node(1);
    check(node.automation_id() == 0, "automationId 0 means unset");
    node.set_automation_id(42);
    check(node.automation_id() == 42, "automationId is read back");
    check(node.dirty() == ui::Dirty::None, "automationId does not invalidate style or layout");
    node.set_automation_id(42);
    check(node.automation_id() == 42, "setting the same automationId is a no-op");

    node.set_role("button");
    check_eq(node.role(), "button", "role is read back");
    check(node.dirty() == ui::Dirty::None, "role does not invalidate style or layout");
    node.set_role(long_text(ui::Node::kMaxMetadataBytes + 4));
    check(node.role().size() == ui::Node::kMaxMetadataBytes, "role is truncated to the byte cap");
    node.clear_role();
    check(node.role().empty(), "clearing the role works");
}

void test_aria_pairs() {
    ui::Node node(1);
    check(node.aria_attributes().empty(), "a fresh node has no aria pairs");

    check(node.set_aria("aria-label", "close"), "adding an aria pair succeeds");
    check(node.aria_attributes().size() == 1, "one pair is stored");
    check_eq(node.aria_attributes()[0].name, "aria-label", "aria name is stored");
    check_eq(node.aria_attributes()[0].value, "close", "aria value is stored");
    check(node.dirty() == ui::Dirty::None, "aria metadata does not invalidate style or layout");

    check(node.set_aria("aria-label", "open"), "replacing an aria value succeeds");
    check(node.aria_attributes().size() == 1, "replacing does not add a pair");
    check_eq(node.aria_attributes()[0].value, "open", "the value is replaced");

    check(!node.set_aria("", "x"), "an empty aria name is rejected");
    // Metadata views are borrowed: keep the test storage alive for the life of the node.
    std::vector<std::string> names;
    for (std::size_t i = 1; i < ui::Node::kMaxAriaPairs; ++i) {
        names.push_back("aria-p" + std::to_string(i));
        check(node.set_aria(names.back(), "v"), "pairs below the cap are accepted");
    }
    check(node.aria_attributes().size() == ui::Node::kMaxAriaPairs, "the pair cap is stored");
    check(!node.set_aria("aria-overflow", "v"), "a pair beyond the cap is rejected");

    node.clear_aria();
    check(node.aria_attributes().empty(), "clearing the aria set works");

    std::string long_name = long_text(ui::Node::kMaxAriaNameBytes + 4);
    node.set_aria(long_name, "v");
    check(node.aria_attributes()[0].name.size() == ui::Node::kMaxAriaNameBytes,
          "aria names are truncated to the name cap");
    std::string long_value = long_text(ui::Node::kMaxAriaValueBytes + 4);
    node.set_aria("aria-long", long_value);
    check(node.aria_attributes()[1].value.size() == ui::Node::kMaxAriaValueBytes,
          "aria values are truncated to the value cap");
}

} // namespace

int main() {
    test_element_kind();
    test_id_and_class_dirty();
    test_automation_and_role();
    test_aria_pairs();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "node metadata tests passed\n";
    return 0;
}
