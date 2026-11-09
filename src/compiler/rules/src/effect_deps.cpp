// SPDX-License-Identifier: MIT

// Effect dependency arrays are exact (docs/specs/component-model-v1.md section 2): every prop,
// state variable or animated value the effect body reads is listed, and nothing else is.

#include "expr_walk.hpp"
#include "reporter.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

using jsx::Expr;
using jsx::ExprKind;

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

bool is_dependency_kind(SymbolKind kind) {
    return kind == SymbolKind::Prop || kind == SymbolKind::State ||
           kind == SymbolKind::AnimatedValue;
}

// Adds what a read of `name` requires. Locals are inlined by lowering (decision 8), so a read of
// a local requires what the local reads. `visiting` stops a local that mentions itself.
void add_required(const Context& ctx, const std::string& name, std::vector<std::string>& required,
                  std::vector<std::string>& visiting) {
    const std::string props_prefix = ctx.item.props_name + ".";
    if (!ctx.item.props_name.empty() && name.starts_with(props_prefix)) {
        if (!contains(required, name)) {
            required.push_back(name);
        }
        return;
    }
    const Symbol* symbol = ctx.out.find(name);
    if (symbol == nullptr) {
        return;
    }
    if (is_dependency_kind(symbol->kind)) {
        if (!contains(required, name)) {
            required.push_back(name);
        }
        return;
    }
    if (symbol->kind != SymbolKind::Local || symbol->statement == kNoStatement ||
        contains(visiting, name)) {
        return;
    }
    const auto& init = ctx.item.body[symbol->statement].init;
    if (!init) {
        return;
    }
    visiting.push_back(name);
    ReadOptions options;
    options.props_name = ctx.item.props_name;
    for (const auto& read : collect_reads(*init, options)) {
        add_required(ctx, read.name, required, visiting);
    }
    visiting.pop_back();
}

std::vector<std::string> required_names(const Context& ctx, const Expr& body) {
    ReadOptions options;
    options.props_name = ctx.item.props_name;
    std::vector<std::string> required;
    std::vector<std::string> visiting;
    for (const auto& read : collect_reads(body, options)) {
        add_required(ctx, read.name, required, visiting);
    }
    return required;
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
        if (element->kind != ExprKind::Identifier) {
            ctx.error("TESSERA_REACT_EFFECT_DEPS",
                      "an effect dependency must be a plain name, not an expression", element->span,
                      "list the prop, state variable or animated value by name");
            continue;
        }
        const std::string& name = element->text;
        effect.dependencies.push_back(name);
        const Symbol* symbol = ctx.out.find(name);
        if (symbol == nullptr || !is_dependency_kind(symbol->kind)) {
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
    effect.reads = required_names(ctx, *effect.body);
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
