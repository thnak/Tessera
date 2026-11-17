// SPDX-License-Identifier: MIT

// Issue #463 (docs/design/css-isolation.md): a `className` that reads a CSS module is a constant.
// Every `styles.<class>` folds to the scoped name the scan produced, and the whole expression must
// fold to one string; anything the compiler cannot evaluate is `TESSERA_JSX_STYLES_DYNAMIC`
// (docs/specs/jsx-subset-v1.md section 4). This is deliberately a fold, not an expression lowering:
// the IR keeps the class name the selector index will match.

#include "node_lowering_internal.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::lowering::internal {
namespace {

constexpr std::string_view kDynamic = "TESSERA_JSX_STYLES_DYNAMIC";
constexpr std::string_view kUnknownClass = "TESSERA_CSS_MODULE_UNKNOWN_CLASS";

// A folded value. `folded` is false for anything that is not a string the compiler knows.
struct Folded {
    bool folded = false;
    std::string text;
};

const CssModuleBinding* find_binding(const NodeState& state, std::string_view name) {
    for (const CssModuleBinding& binding : state.context.css_modules) {
        if (binding.binding == name) {
            return &binding;
        }
    }
    return nullptr;
}

// True when the expression names a CSS module binding anywhere, including as an object of a member
// access or a spread source. A `className` with no such name stays entirely on the ordinary
// expression path, so a state read or a literal is unaffected by this slice.
bool reads_css_module(const NodeState& state, const jsx::Expr& expr) {
    if (expr.kind == jsx::ExprKind::Identifier && find_binding(state, expr.text) != nullptr) {
        return true;
    }
    for (const jsx::ExprPtr& operand : expr.operands) {
        if (operand && reads_css_module(state, *operand)) {
            return true;
        }
    }
    // `{...styles}` is an object literal, so its reads live in the properties, not the operands.
    for (const jsx::ObjectProperty& property : expr.properties) {
        if (property.value && reads_css_module(state, *property.value)) {
            return true;
        }
    }
    return false;
}

// `styles.<ident>` in a template or in `+` concatenation is the only foldable shape; every other
// node returns an unfolded value and the caller reports the dynamic diagnostic.
Folded fold(NodeState& state, const jsx::Expr& expr) {
    switch (expr.kind) {
    case jsx::ExprKind::String:
        return Folded{true, expr.text};
    case jsx::ExprKind::Member: {
        if (expr.operands.empty() || !expr.operands[0] ||
            expr.operands[0]->kind != jsx::ExprKind::Identifier) {
            return {};
        }
        const CssModuleBinding* module = find_binding(state, expr.operands[0]->text);
        if (module == nullptr) {
            return {};
        }
        // `styles[k]` has text "[]" and the key as its second operand: its class is only known at
        // run time, and the IR has no scoped-name lookup.
        if (expr.text == "[]") {
            fail_node(state, kDynamic,
                      "`" + module->binding +
                          "[...]` cannot be resolved at compile time; a CSS module class is static",
                      expr.span, "write `styles.card` with the class name written out");
            return {};
        }
        for (const ir::StylesheetClass& declared : module->classes) {
            if (declared.local != expr.text) {
                continue;
            }
            state.css_module_uses.push_back(CssModuleUse{module->path, declared.local});
            return Folded{true, declared.scoped};
        }
        fail_node(state, kUnknownClass,
                  "`" + module->binding + "." + expr.text + "` names no class of `" + module->path +
                      "`",
                  expr.span, "use a class the module file declares");
        return {};
    }
    case jsx::ExprKind::Template: {
        if (expr.template_strings.size() != expr.operands.size() + 1) {
            return {};
        }
        std::string text;
        for (std::size_t i = 0; i < expr.template_strings.size(); ++i) {
            text += expr.template_strings[i];
            if (i == expr.operands.size()) {
                break;
            }
            if (!expr.operands[i]) {
                return {};
            }
            Folded part = fold(state, *expr.operands[i]);
            if (!part.folded) {
                return {};
            }
            text += part.text;
        }
        return Folded{true, std::move(text)};
    }
    case jsx::ExprKind::Binary: {
        if (expr.text != "+" || expr.operands.size() != 2 || !expr.operands[0] ||
            !expr.operands[1]) {
            return {};
        }
        Folded left = fold(state, *expr.operands[0]);
        if (!left.folded) {
            return {};
        }
        Folded right = fold(state, *expr.operands[1]);
        if (!right.folded) {
            return {};
        }
        return Folded{true, std::move(left.text) + right.text};
    }
    default:
        return {};
    }
}

} // namespace

CssModuleClass resolve_css_module_class(NodeState& state, const jsx::Expr& expr) {
    if (!reads_css_module(state, expr)) {
        return {};
    }
    Folded folded = fold(state, expr);
    if (state.error) {
        return CssModuleClass{CssModuleClass::Kind::Error, {}};
    }
    if (folded.folded) {
        return CssModuleClass{CssModuleClass::Kind::Resolved, std::move(folded.text)};
    }
    fail_node(state, kDynamic,
              "a `className` that reads a CSS module must fold to one class string at compile time",
              expr.span, "write `styles.card` or a template of `styles.*` reads only");
    return CssModuleClass{CssModuleClass::Kind::Error, {}};
}

} // namespace tessera::compiler::lowering::internal
