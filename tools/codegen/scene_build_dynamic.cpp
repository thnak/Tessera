// SPDX-License-Identifier: MIT

#include "scene_build_dynamic.hpp"

#include <utility>

namespace tessera::compiler::codegen::cli::detail {
namespace {

namespace eval = tessera::compiler::eval;

std::string unsupported(std::string what) {
    return "TESSERA_SCENE_UNSUPPORTED: " + std::move(what);
}

} // namespace

bool evaluate_condition(eval::Evaluator& evaluator, const eval::Scope& scope, const ir::Node& node,
                        bool& show, std::string& error) {
    if (!node.condition) {
        error = "the preview asked for a condition on `" + node.id + "`, which has none";
        return false;
    }
    auto value = evaluator.evaluate(*node.condition, scope);
    if (!value) {
        error = unsupported("the condition of `" + node.id + "` cannot be evaluated: " +
                            value.error().code + ": " + value.error().message);
        return false;
    }
    // A condition is boolean-valued (ir-v1 section 3), and the evaluator's own `Conditional` kind
    // rejects a non-bool test the same way. Coercing here would make the preview show a branch the
    // language says is a type error, so a non-bool is reported instead of guessed at.
    const bool* flag = value->as_bool();
    if (flag == nullptr) {
        error = unsupported("the condition of `" + node.id + "` is a " +
                            std::string(eval::type_name(*value)) + ", not a bool");
        return false;
    }
    show = *flag;
    return true;
}

bool evaluate_items(eval::Evaluator& evaluator, const eval::Scope& scope, const ir::Node& node,
                    eval::List& out, std::string& error) {
    out.clear();
    if (!node.loop || !node.loop->source) {
        error = "the preview asked for items of `" + node.id + "`, which carries no loop";
        return false;
    }
    auto value = evaluator.evaluate(*node.loop->source, scope);
    if (!value) {
        error = unsupported("the list source of `" + node.id + "` cannot be evaluated: " +
                            value.error().code + ": " + value.error().message);
        return false;
    }
    const eval::List* items = value->as_list();
    if (items == nullptr) {
        // A source that is not a list is an IR the validator rejects outright (ir-v1 section 5);
        // naming the type is what makes "the preview disagrees with the compiler" attributable
        // instead of a silent empty list.
        error = unsupported("the list source of `" + node.id + "` is a " +
                            std::string(eval::type_name(*value)) +
                            ", not a list, so the repeated child has no items");
        return false;
    }
    if (items->size() > kMaxPreviewItems) {
        error = "the repeated child `" + node.id + "` has " + std::to_string(items->size()) +
                " items, more than the preview paints (" + std::to_string(kMaxPreviewItems) + ")";
        return false;
    }
    out = *items;
    return true;
}

bool evaluate_item_key(eval::Evaluator& evaluator, const eval::Scope& scope, const ir::Node& node,
                       bool& present, std::uint64_t& key, std::string& error) {
    present = false;
    key = 0;
    if (!node.key) {
        // Every looped node has a key (ir-v1 section 5 requires one), so reaching here without one
        // is an internal error; an item with no identity cannot be told apart from another item.
        error = "the repeated child `" + node.id + "` has no key";
        return false;
    }
    auto value = evaluator.evaluate(*node.key, scope);
    if (!value) {
        error =
            unsupported("the key of `" + node.id + "` cannot be evaluated: " + value.error().code +
                        ": " + value.error().message);
        return false;
    }
    // Generated code publishes the key on the item root only when the key's C++ form is an integer
    // (`numeric_item_key()` in emit_list.cpp): the loop index, an `int` item, or an integer
    // literal. A string key is matched by the generated key table instead, and carries no `itemKey`
    // here either, so the two sides agree on which items have a numeric identity.
    const std::int32_t* whole = value->as_int();
    if (whole == nullptr) {
        return true;
    }
    present = true;
    key = static_cast<std::uint64_t>(static_cast<std::int64_t>(*whole));
    return true;
}

} // namespace tessera::compiler::codegen::cli::detail
