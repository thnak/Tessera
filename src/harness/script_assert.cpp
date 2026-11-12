// SPDX-License-Identifier: MIT

#include "script_internal.hpp"

#include <string>
#include <string_view>

namespace tessera::harness::detail {
namespace {

const JsonValue* find_node(const JsonValue& snapshot, const ScriptStep& step) {
    const JsonValue* nodes = snapshot.find("nodes");
    const JsonValue::Array* list = nodes == nullptr ? nullptr : nodes->as_array();
    if (list == nullptr || !step.ui_id.has_value()) {
        return nullptr;
    }
    const std::string wanted = std::to_string(*step.ui_id);
    for (const JsonValue& node : *list) {
        const JsonValue* ui_id = node.find("uiId");
        const auto id = ui_id == nullptr ? std::nullopt : ui_id->as_string();
        if (!id.has_value() || *id != wanted) {
            continue;
        }
        if (step.item_key.has_value()) {
            const JsonValue* key = node.find("itemKey");
            const auto text = key == nullptr ? std::nullopt : key->as_string();
            if (!text.has_value() || *text != *step.item_key) {
                continue;
            }
        }
        return &node;
    }
    return nullptr;
}

std::optional<std::string> check_flag(const JsonValue& node, std::string_view key,
                                      const std::optional<bool>& expected) {
    if (!expected.has_value()) {
        return std::nullopt;
    }
    const JsonValue* member = node.find(key);
    const auto actual = member == nullptr ? std::nullopt : member->as_bool();
    if (actual == expected) {
        return std::nullopt;
    }
    return std::string(key) + " is " + (actual.value_or(false) ? "true" : "false") + ", expected " +
           (*expected ? "true" : "false");
}

std::optional<std::string> check_text(const JsonValue& node, std::string_view key,
                                      const std::optional<std::string>& expected) {
    if (!expected.has_value()) {
        return std::nullopt;
    }
    const JsonValue* member = node.find(key);
    const auto actual = member == nullptr ? std::nullopt : member->as_string();
    if (actual.has_value() && *actual == *expected) {
        return std::nullopt;
    }
    return std::string(key) + " is \"" + std::string(actual.value_or("")) + "\", expected \"" +
           *expected + "\"";
}

} // namespace

std::optional<std::string> evaluate_assertion(const ScriptStep& step, const JsonValue& snapshot) {
    const JsonValue* node = find_node(snapshot, step);
    if (node == nullptr) {
        // `exists: false` is the only expectation an absent node satisfies.
        if (step.expect_exists == false) {
            return std::nullopt;
        }
        return std::string("node not found");
    }
    if (step.expect_exists == false) {
        return std::string("node exists, expected it to be absent");
    }
    for (const auto& failure : {check_flag(*node, "visible", step.expect_visible),
                                check_flag(*node, "enabled", step.expect_enabled),
                                check_flag(*node, "focused", step.expect_focused),
                                check_flag(*node, "selected", step.expect_selected),
                                check_text(*node, "text", step.expect_text),
                                check_text(*node, "name", step.expect_name)}) {
        if (failure.has_value()) {
            return failure;
        }
    }
    return std::nullopt;
}

} // namespace tessera::harness::detail
