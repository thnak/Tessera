// SPDX-License-Identifier: MIT

// `useReducer` in its limited form (ADR-066 item 4):
//
//   type CounterState = { count: int };
//   type CounterAction = { type: "increment" } | { type: "reset" };
//   function reduce(state: CounterState, action: CounterAction): CounterState {
//     switch (action.type) {
//       case "increment": return { count: state.count + 1 };
//       case "reset":     return { count: 0 };
//     }
//   }
//   ...
//   const [state, dispatch] = useReducer(reduce, { count: 0 });
//
// This pass answers one question: is the source written in that form? Everything it needs is
// resolved here — the helper, its two parameters, the closed action union and the `switch` — and
// recorded as a `ReducerInfo`, so lowering turns the record into IR without re-reading the module's
// types. Anything outside the form is `TESSERA_REACT_REDUCER_FORM`, reported at the part that broke
// it rather than at the whole statement.
//
// The point of the restriction is that the form is a *total* function of the action: one state cell
// plus one handler per action type, with no run-time `switch` and no function value. A reducer that
// needs more than that (a payload-carrying action, two switches, a loop) is what a native handler
// is for, and saying so is better than lowering half of it.

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

// The `type` literals an action union declares. `{ type: "a" } | { type: "b" }` gives {a, b}; the
// only member the union may declare is `type`, and its type must be a string literal, because that
// is what the dispatch table is keyed by. Returns nullopt with `why` set when the union is outside
// the form.
struct ActionUnion {
    std::vector<std::string> types;
    jsx::Span span;
};

std::optional<std::string> declared_type_name(const jsx::TypePtr& type) {
    if (!type || type->kind != jsx::TypeKind::Name || !type->children.empty()) {
        return std::nullopt;
    }
    return type->text;
}

// The object members of one alternative of the action union, or null when it is not an object.
const jsx::TypeNode* alternative(const jsx::TypeNode& union_type, std::size_t index) {
    if (index >= union_type.children.size()) {
        return nullptr;
    }
    const jsx::TypeNode* child = union_type.children[index].get();
    return child != nullptr && child->kind == jsx::TypeKind::Object ? child : nullptr;
}

// Resolves `name` to the object body it ultimately declares, following same-file aliases. Only an
// object body is returned: a primitive, array or function alias is not a record.
const jsx::TypeNode* resolve_object_body(const jsx::Module& module, std::string_view name,
                                         std::size_t depth = 0) {
    if (depth > 8) {
        return nullptr;
    }
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind != jsx::ItemKind::TypeAlias || item.name != name || !item.type) {
            continue;
        }
        if (item.type->kind == jsx::TypeKind::Object) {
            return item.type.get();
        }
        const std::optional<std::string> next = declared_type_name(item.type);
        return next ? resolve_object_body(module, *next, depth + 1) : nullptr;
    }
    return nullptr;
}

// The action union `name` declares, or nullopt when it is not one the form accepts. A closed union
// of object literals each carrying a `type` string literal is the only accepted shape; `why` names
// the reason for the diagnostic.
std::optional<ActionUnion> read_action_union(const Context& ctx, std::string_view name,
                                             std::string& why) {
    const jsx::TypeNode* type = nullptr;
    for (const jsx::ModuleItem& item : ctx.module.items) {
        if (item.kind == jsx::ItemKind::TypeAlias && item.name == name && item.type) {
            type = item.type.get();
            break;
        }
    }
    if (type == nullptr) {
        why = "`" + std::string(name) + "` is not a type declared in this module";
        return std::nullopt;
    }
    if (type->kind != jsx::TypeKind::Union) {
        why = "`" + std::string(name) + "` is not a union of object types";
        return std::nullopt;
    }
    ActionUnion result;
    result.span = type->span;
    for (std::size_t i = 0; i < type->children.size(); ++i) {
        const jsx::TypeNode* branch = alternative(*type, i);
        if (branch == nullptr) {
            why = "every alternative of `" + std::string(name) + "` must be an object type";
            return std::nullopt;
        }
        const jsx::TypeMember* type_member = nullptr;
        for (const jsx::TypeMember& member : branch->members) {
            if (member.name == "type") {
                type_member = &member;
                break;
            }
        }
        if (type_member == nullptr || !type_member->type) {
            why = "every alternative of `" + std::string(name) + "` needs a `type` member";
            return std::nullopt;
        }
        if (branch->members.size() != 1) {
            // A payload-carrying action is the feature the limited form does not have: the
            // generated handler would have to carry the value, and the dispatch table is keyed by
            // the `type` literal alone.
            why = "an alternative of `" + std::string(name) +
                  "` has members besides `type`; a dispatch in the limited form carries no value";
            return std::nullopt;
        }
        if (type_member->type->kind != jsx::TypeKind::StringLiteral) {
            why = "the `type` member of `" + std::string(name) + "` must be a string literal";
            return std::nullopt;
        }
        result.types.push_back(type_member->type->text);
    }
    if (result.types.empty()) {
        why = "`" + std::string(name) + "` declares no action type";
        return std::nullopt;
    }
    return result;
}

// A `case` label: the string-literal expression the parser recorded as the block's first operand.
// A `default` block has no label, so its first operand is already a statement.
const jsx::Expr* case_label(const Expr& block) {
    if (block.operands.empty()) {
        return nullptr;
    }
    const jsx::Expr* first = block.operands.front().get();
    return first != nullptr && first->kind == ExprKind::String ? first : nullptr;
}

// The single `return <record>;` of a case body, or null when the body is anything else. A reducer
// case is exactly one `return` of a record value, so a statement list with a local, an `if` or two
// returns is outside the form. `labelled` says whether the block starts with a label.
const Expr* case_return_value(const Expr& block, bool labelled) {
    const std::size_t expected = labelled ? 2 : 1;
    if (block.kind != ExprKind::Block || block.operands.size() != expected) {
        return nullptr;
    }
    const Expr& statement = *block.operands[expected - 1];
    if (statement.kind != ExprKind::Return || statement.operands.size() != 1 ||
        !statement.operands[0]) {
        return nullptr;
    }
    return statement.operands[0].get();
}

// Whether `expr` is the switch subject the form requires: a field read of the action parameter,
// which is how `action.type` reaches the union's `type` member.
bool is_action_type_subject(const Expr& subject, std::string_view action_param) {
    return subject.kind == ExprKind::Member && subject.text == "type" &&
           subject.operands.size() == 1 && subject.operands[0] &&
           subject.operands[0]->kind == ExprKind::Identifier &&
           subject.operands[0]->text == action_param;
}

// The `switch` of the reducer body, or null when the body is not exactly one switch.
const Expr* reducer_switch(const jsx::ModuleItem& helper) {
    const Expr* found = nullptr;
    for (const jsx::Statement& statement : helper.body) {
        if (!statement.init) {
            return nullptr;
        }
        if (statement.init->kind != ExprKind::Switch) {
            return nullptr; // a reducer body is one statement, and it is the switch
        }
        if (found != nullptr) {
            return nullptr;
        }
        found = statement.init.get();
    }
    return found;
}

void fail(Context& ctx, std::string message, jsx::Span span) {
    ctx.error("TESSERA_REACT_REDUCER_FORM", std::move(message), span,
              "write `function reduce(state: S, action: A): S { switch (action.type) { case "
              "\"x\": return {...}; } }`, where `A` is a union of `{ type: \"x\" }` objects");
}

// The helper `name` names, or null when the module does not declare it.
const jsx::ModuleItem* find_helper(const jsx::Module& module, std::string_view name) {
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind == jsx::ItemKind::Helper && item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

void check_use_reducer(Context& ctx, std::size_t index, const jsx::Statement& statement,
                       const Expr& call) {
    const auto reject_form = [&](const std::string& message, jsx::Span span) {
        ctx.error("TESSERA_REACT_REDUCER_FORM", message, span,
                  "write `const [state, dispatch] = useReducer(reduce, initial);` with a module "
                  "helper whose body is one `switch` on the action type");
    };
    if (statement.kind != jsx::StatementKind::Const || !statement.array_pattern ||
        statement.names.size() != 2 || call.operands.size() != 3) {
        reject_form("`useReducer` is `const [state, dispatch] = useReducer(reduce, initial);`",
                    statement.span);
        return;
    }
    const Expr* reducer_ref = call.operands[1].get();
    if (reducer_ref == nullptr || reducer_ref->kind != ExprKind::Identifier) {
        reject_form("the first argument of `useReducer` must be the name of a module-scope helper",
                    call.span);
        return;
    }
    const jsx::ModuleItem* helper = find_helper(ctx.module, reducer_ref->text);
    if (helper == nullptr) {
        reject_form(
            "`" + reducer_ref->text +
                "` is not a module-scope `function` declared in this file; the limited form "
                "needs its body to lower",
            reducer_ref->span);
        return;
    }
    if (helper->helper_params.size() != 2) {
        reject_form("reducer `" + helper->name + "` must take `(state, action)`",
                    helper->name_span);
        return;
    }
    const jsx::HelperParam& state_param = helper->helper_params[0];
    const jsx::HelperParam& action_param = helper->helper_params[1];
    const std::optional<std::string> state_type = declared_type_name(state_param.type);
    const std::optional<std::string> action_type = declared_type_name(action_param.type);
    if (!state_type || !action_type) {
        reject_form("both parameters of reducer `" + helper->name +
                        "` need a declared type name, so the state and action records are known",
                    helper->name_span);
        return;
    }
    if (resolve_object_body(ctx.module, *state_type) == nullptr) {
        reject_form("the state type `" + *state_type + "` of reducer `" + helper->name +
                        "` is not a record declared in this module",
                    helper->name_span);
        return;
    }
    std::string why;
    const std::optional<ActionUnion> union_type = read_action_union(ctx, *action_type, why);
    if (!union_type) {
        reject_form(why, helper->name_span);
        return;
    }
    // The returned record of a case is the state type, so the reducer is a total function from the
    // action union back to the state.
    const std::optional<std::string> return_type = declared_type_name(helper->type);
    if (!return_type || *return_type != *state_type) {
        reject_form("reducer `" + helper->name + "` must return its state type `" + *state_type +
                        "`",
                    helper->name_span);
        return;
    }
    const Expr* switch_statement = reducer_switch(*helper);
    if (switch_statement == nullptr || switch_statement->operands.empty()) {
        reject_form("the body of reducer `" + helper->name + "` must be one `switch`",
                    helper->span);
        return;
    }
    if (!is_action_type_subject(*switch_statement->operands[0], action_param.name)) {
        reject_form("reducer `" + helper->name + "` must switch on `" + action_param.name +
                        ".type`",
                    switch_statement->span);
        return;
    }
    ReducerInfo info;
    info.statement = index;
    info.name = statement.names[0];
    info.dispatch = statement.names[1];
    info.reducer = helper->name;
    info.state_type = *state_type;
    // Each case must name one of the union's `type` literals, and each must return a record of the
    // state type. A case the union does not declare is a literal the dispatch table could never
    // hold, so it is a form error rather than a `default`.
    std::vector<std::string> seen;
    for (std::size_t i = 1; i < switch_statement->operands.size(); ++i) {
        const Expr* block = switch_statement->operands[i].get();
        if (block == nullptr || block->kind != ExprKind::Block) {
            reject_form("every `case` of reducer `" + helper->name + "` must be a block",
                        switch_statement->span);
            return;
        }
        const Expr* label = case_label(*block);
        const Expr* returned = case_return_value(*block, label != nullptr);
        if (returned == nullptr) {
            reject_form("every `case` of reducer `" + helper->name +
                            "` must be one `return <record>;`",
                        block->span);
            return;
        }
        if (returned->kind != ExprKind::Object) {
            reject_form("every `case` of reducer `" + helper->name + "` must return a record of `" +
                            *state_type + "`",
                        returned->span);
            return;
        }
        if (label == nullptr) {
            // A `default` clause. The form has no case outside the union to fall to, so a default
            // is a second way to say the same thing and the dispatch table has no key for it.
            info.has_default = true;
            continue;
        }
        if (label->kind != ExprKind::String) {
            reject_form("a `case` label of reducer `" + helper->name +
                            "` must be a string literal, because the dispatch table is keyed by it",
                        label->span);
            return;
        }
        const bool declared = std::find(union_type->types.begin(), union_type->types.end(),
                                        label->text) != union_type->types.end();
        if (!declared) {
            reject_form("`" + label->text + "` is not a `type` of `" + *action_type + "`",
                        label->span);
            return;
        }
        const bool repeated =
            std::find_if(info.cases.begin(), info.cases.end(), [&](const ReducerCase& existing) {
                return existing.action == label->text;
            }) != info.cases.end();
        if (repeated) {
            reject_form("`" + label->text + "` is handled twice by reducer `" + helper->name + "`",
                        label->span);
            return;
        }
        info.cases.push_back(ReducerCase{label->text, label->span, returned});
    }
    // A missing case is not a fall-through: the generated handler set would have no handler for
    // that action, so `dispatch({ type: "x" })` would compile and do nothing. Report it as a form
    // error.
    for (const std::string& declared : union_type->types) {
        const bool handled =
            std::find_if(info.cases.begin(), info.cases.end(), [&](const ReducerCase& existing) {
                return existing.action == declared;
            }) != info.cases.end();
        if (!handled) {
            reject_form("reducer `" + helper->name + "` does not handle the action `" + declared +
                            "` that `" + *action_type + "` declares",
                        helper->name_span);
            return;
        }
    }
    for (const std::string& name : statement.names) {
        add_symbol(ctx, name, name == info.name ? SymbolKind::State : SymbolKind::Dispatch,
                   statement.span, index);
    }
    ctx.out.reducers.push_back(std::move(info));
}

} // namespace

// Whether `expr` is a dispatch call of the component's `useReducer` state (ADR-066 item 4), which a
// handler body may hold like any other setter call.
bool is_dispatch_call(const Context& ctx, const Expr& expr) {
    if (expr.kind != ExprKind::Call || expr.operands.empty() || !expr.operands[0]) {
        return false;
    }
    const Expr& callee = *expr.operands[0];
    if (callee.kind != ExprKind::Identifier) {
        return false;
    }
    const Symbol* symbol = ctx.out.find(callee.text);
    return symbol != nullptr && symbol->kind == SymbolKind::Dispatch;
}

void check_reducers(Context& ctx) {
    for (std::size_t index = 0; index < ctx.item.body.size(); ++index) {
        const jsx::Statement& statement = ctx.item.body[index];
        if (statement.init && callee_name(*statement.init) == "useReducer") {
            check_use_reducer(ctx, index, statement, *statement.init);
        }
    }
}

} // namespace tessera::compiler::rules
