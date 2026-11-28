// SPDX-License-Identifier: MIT

// Effect dependency arrays are exact (docs/specs/component-model-v1.md section 2): every prop,
// state variable or animated value the effect body reads is listed, and nothing else is.

#include "dependency_names.hpp"
#include "reporter.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string_view kind_text(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::StateSetter:
        return "a state setter";
    case SymbolKind::Ref:
        return "a ref";
    case SymbolKind::Handler:
        return "a handler";
    case SymbolKind::Local:
        return "a local constant";
    case SymbolKind::PropsParam:
        return "the props object";
    default:
        break;
    }
    return "not a dependency";
}

// Checks each listed entry; fills `listed` with the valid names.
void check_listed(Context& ctx, EffectInfo& effect, const Expr& array,
                  const std::vector<std::string>& required) {
    for (const auto& element : array.operands) {
        // A resource field (`feed.status`, `feed.data`, `feed.error`) is the one dotted form.
        const bool member = element->kind == ExprKind::Member && element->operands.size() == 1 &&
                            element->operands[0] &&
                            element->operands[0]->kind == ExprKind::Identifier;
        if (element->kind != ExprKind::Identifier && !member) {
            ctx.error("TESSERA_REACT_EFFECT_DEPS",
                      "an effect dependency must be a plain name, not an expression", element->span,
                      "list the prop, state variable or animated value by name");
            continue;
        }
        const std::string name =
            member ? element->operands[0]->text + "." + element->text : element->text;
        effect.dependencies.push_back(name);
        if (member) {
            if (!is_resource_read(ctx, name)) {
                ctx.error("TESSERA_REACT_EFFECT_DEPS",
                          "`" + name +
                              "` is not a dependency; only `status`, `data` and `error` of a "
                              "`useResource` binding can be listed with a dot",
                          element->span,
                          "list props, state variables, animated values and resource fields only");
                continue;
            }
        }
        const Symbol* symbol = member ? nullptr : ctx.out.find(name);
        if (!member && (symbol == nullptr || !is_dependency_kind(symbol->kind))) {
            const std::string what = symbol == nullptr
                                         ? "is not declared in this component"
                                         : "is " + std::string(kind_text(symbol->kind));
            ctx.error("TESSERA_REACT_EFFECT_DEPS",
                      "`" + name + "` " + what + ", so it cannot be an effect dependency",
                      element->span, "list props, state variables and animated values only");
            continue;
        }
        const auto first = std::find(effect.dependencies.begin(), effect.dependencies.end(), name);
        if (first != effect.dependencies.end() - 1) {
            ctx.error("TESSERA_REACT_EFFECT_DEPS", "`" + name + "` is listed twice", element->span,
                      "remove the duplicate");
        } else if (!contains(required, name)) {
            ctx.error("TESSERA_REACT_EFFECT_DEPS",
                      "`" + name + "` is listed but the effect body does not read it",
                      element->span, "remove `" + name + "` from the dependency array");
        }
    }
}

void check_effect(Context& ctx, EffectInfo& effect) {
    effect.reads = required_names(ctx, *effect.body, true);
    if (effect.dependency_array == nullptr) {
        ctx.error("TESSERA_REACT_EFFECT_DEPS",
                  "`useEffect` needs a dependency array (V1 dependencies are exact)",
                  ctx.item.body[effect.statement].span, "add `[]`, or list what the effect reads");
        return;
    }
    const Expr& array = *effect.dependency_array;
    if (array.kind != ExprKind::Array) {
        ctx.error("TESSERA_REACT_EFFECT_DEPS",
                  "the effect dependencies must be an array literal of names", array.span,
                  "write `[a, b]`");
        return;
    }
    check_listed(ctx, effect, array, effect.reads);
    for (const auto& name : effect.reads) {
        if (contains(effect.dependencies, name)) {
            continue;
        }
        const bool props_member =
            !ctx.item.props_name.empty() && name.starts_with(ctx.item.props_name + ".");
        ctx.error("TESSERA_REACT_EFFECT_DEPS",
                  "the effect reads `" + name + "` but does not list it as a dependency",
                  array.span,
                  props_member ? "destructure the props parameter so the prop has a name to list"
                               : "add `" + name + "` to the dependency array");
    }
}

} // namespace

void check_effect_deps(Context& ctx) {
    for (auto& effect : ctx.out.effects) {
        check_effect(ctx, effect);
    }
}

} // namespace tessera::compiler::rules
