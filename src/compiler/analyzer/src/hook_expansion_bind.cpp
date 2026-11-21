// SPDX-License-Identifier: MIT

// Binding the results of a custom-hook call (spike #659, ADR-065; docs/specs/custom-hooks-v1.md
// section 4 step 6). The call-site pattern decides how the hook's `return` value is read:
//
// - an array return needs an array pattern of the same length (`const [a, b] = usePair();`);
// - an object return takes an object pattern (`const { page, next: go } = usePagination();`) or a
//   single whole-value name (`const p = usePagination();`, where `p.page` resolves and bare `p`
//   does not, spec section 3);
// - any other return needs one non-array name.
//
// The returned expression is renamed and substituted first, so `return { page }` yields the hook's
// own `usePagination_1__page` and not the declaration's `page`.

#include "hook_expansion_internal.hpp"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx::internal {

std::optional<std::string> bind_results(const ModuleItem& hook, const Statement& statement,
                                        const std::map<std::string, std::string>& renames,
                                        const Substitutions& parameters, Substitutions& results) {
    if (!hook.hook_return) {
        return std::nullopt;
    }
    ExprPtr returned = clone_expr(*hook.hook_return);
    rename(*returned, renames);
    std::vector<Span> whole_value_uses;
    substitute(returned, parameters, whole_value_uses);

    if (returned->kind == ExprKind::Array) {
        if (!statement.array_pattern || statement.names.size() != returned->operands.size()) {
            return "`" + hook.name + "` returns " + std::to_string(returned->operands.size()) +
                   " value(s); bind them with an array pattern of that length";
        }
        for (std::size_t i = 0; i < statement.names.size(); ++i) {
            results.values[statement.names[i]] = clone_expr(*returned->operands[i]);
        }
        return std::nullopt;
    }
    if (returned->kind == ExprKind::Object) {
        if (!statement.object_pattern.empty()) {
            for (const PatternBinding& binding : statement.object_pattern) {
                const ExprPtr* field = nullptr;
                for (const ObjectProperty& property : returned->properties) {
                    if (property.key == binding.key) {
                        field = &property.value;
                        break;
                    }
                }
                if (field == nullptr) {
                    return "`" + hook.name + "` does not return a field `" + binding.key + "`";
                }
                results.values[binding.local] = clone_expr(**field);
            }
            return std::nullopt;
        }
        if (statement.names.size() == 1) {
            // A whole-value binding: `p.page` resolves, `p` alone does not (spec section 3).
            std::map<std::string, ExprPtr> fields;
            for (const ObjectProperty& property : returned->properties) {
                fields[property.key] = clone_expr(*property.value);
            }
            results.objects[statement.names.front()] = std::move(fields);
            return std::nullopt;
        }
        return "`" + hook.name +
               "` returns an object; bind it as a whole or with an object pattern";
    }
    if (statement.names.size() == 1 && !statement.array_pattern) {
        results.values[statement.names.front()] = std::move(returned);
        return std::nullopt;
    }
    if (!statement.names.empty()) {
        return "`" + hook.name + "` returns one value; bind it with a single name";
    }
    return std::nullopt;
}

} // namespace tessera::compiler::jsx::internal
