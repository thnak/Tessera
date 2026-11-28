// SPDX-License-Identifier: MIT

// The shim pass (docs/specs/router-shim-v1.md section 4): import checks against the coverage
// tables, and the `useNavigate` rewrite.
//
//   const navigate = useNavigate();      ->  const navigate = useService<RouterService>();
//   navigate("/orders")                  ->  navigate.push("/orders")
//   navigate(path, { replace: true })    ->  navigate.replace(path)
//   navigate(-1)                         ->  navigate.back()
//
// The rewritten module is ordinary subset code: the service rules check the call (handler or effect
// only, never while rendering), lowering emits the usual `callService` action and code generation
// the typed accessor, so the shim adds no IR kind and no run-time cost.

#include "shim_table.hpp"
#include "tessera/compiler/jsx/shim_expansion.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace tessera::compiler::jsx {
namespace {

constexpr std::string_view kForm = "TESSERA_SHIM_FORM";
constexpr std::string_view kPending = "TESSERA_SHIM_PENDING";
constexpr std::string_view kUnsupported = "TESSERA_SHIM_UNSUPPORTED";
constexpr std::string_view kRouterService = "RouterService";

class ShimPass {
public:
    ShimPass(Module& module, DiagnosticSink& sink)
        : module_(module),
          sink_(sink) {}

    void run() {
        check_imports();
        if (navigate_local_.empty()) {
            return;
        }
        for (ModuleItem& item : module_.items) {
            if (item.kind == ItemKind::Component) {
                rewrite_component(item);
            } else if (item.kind == ItemKind::Hook) {
                reject_in_hook(item);
            }
        }
    }

private:
    void check_imports() {
        for (const ModuleItem& item : module_.items) {
            if (item.kind != ItemKind::Import || !is_shim_package(item.source)) {
                continue;
            }
            for (const ImportName& name : item.names) {
                check_name(item.source, name);
            }
        }
        std::erase_if(module_.items, [](const ModuleItem& item) {
            return item.kind == ItemKind::Import && is_shim_package(item.source);
        });
    }

    void check_name(const std::string& package, const ImportName& name) {
        const shim::Row* row = shim::find_row(package, name.name);
        const std::string where = "`" + excerpt(name.name) + "` of `" + excerpt(package) + "`";
        if (row == nullptr) {
            sink_.error(kUnsupported, where + " is not in the shim's coverage table", name.span,
                        std::string(shim::unlisted_hint(package)));
            return;
        }
        switch (row->status) {
        case shim::Status::Supported:
            if (package == "react-router-dom" || package == "react-router") {
                if (row->name == shim::kNavigateHook) {
                    navigate_local_ = std::string(name.local());
                }
            }
            break;
        case shim::Status::Pending:
            sink_.error(kPending, where + " is covered by the shim but its lowering has not landed",
                        name.span, std::string(row->hint));
            break;
        case shim::Status::Unsupported:
            sink_.error(kUnsupported, where + " is outside the shim", name.span,
                        std::string(row->hint));
            break;
        }
    }

    [[nodiscard]] bool is_navigate_hook_call(const Expr& expr) const {
        return expr.kind == ExprKind::Call && !expr.operands.empty() && expr.operands[0] &&
               expr.operands[0]->kind == ExprKind::Identifier &&
               expr.operands[0]->text == navigate_local_;
    }

    void reject_in_hook(const ModuleItem& item) {
        for (const Statement& statement : item.body) {
            if (statement.init && is_navigate_hook_call(*statement.init)) {
                sink_.error(kForm, "`useNavigate` is not accepted inside a custom hook",
                            statement.span,
                            "call `useNavigate` in the component and pass a callback to the hook");
            }
        }
    }

    void rewrite_component(ModuleItem& item) {
        navigate_names_.clear();
        for (Statement& statement : item.body) {
            if (!statement.init || !is_navigate_hook_call(*statement.init)) {
                continue;
            }
            Expr& call = *statement.init;
            const bool plain = statement.kind == StatementKind::Const && !statement.array_pattern &&
                               statement.object_pattern.empty() && statement.names.size() == 1 &&
                               call.operands.size() == 1 && call.type_args.empty();
            if (!plain) {
                sink_.error(kForm, "`useNavigate` is not written in its accepted form",
                            statement.span, "write `const navigate = useNavigate();`");
                continue;
            }
            call.operands[0]->text = "useService";
            auto type = std::make_unique<TypeNode>();
            type->kind = TypeKind::Name;
            type->span = call.operands[0]->span;
            type->text = std::string(kRouterService);
            call.type_args.push_back(std::move(type));
            navigate_names_.insert(statement.names.front());
        }
        if (navigate_names_.empty()) {
            return;
        }
        for (Statement& statement : item.body) {
            if (statement.init) {
                rewrite_expr(*statement.init);
            }
            std::erase_if(statement.memo_deps, [this](const DependencyName& dependency) {
                return navigate_names_.count(dependency.name) != 0;
            });
        }
        if (item.returned) {
            rewrite_jsx(*item.returned);
        }
    }

    void rewrite_jsx(JsxNode& node) {
        for (JsxAttribute& attribute : node.attributes) {
            if (attribute.expr) {
                rewrite_expr(*attribute.expr);
            }
        }
        for (JsxPtr& child : node.children) {
            if (child) {
                rewrite_jsx(*child);
            }
        }
        if (node.expr) {
            rewrite_expr(*node.expr);
        }
    }

    void rewrite_expr(Expr& expr) {
        for (ExprPtr& operand : expr.operands) {
            if (operand) {
                rewrite_expr(*operand);
            }
        }
        for (ObjectProperty& property : expr.properties) {
            if (property.value) {
                rewrite_expr(*property.value);
            }
        }
        if (expr.spread_base) {
            rewrite_expr(*expr.spread_base);
        }
        if (expr.jsx) {
            rewrite_jsx(*expr.jsx);
        }
        if (expr.kind == ExprKind::Call && !expr.operands.empty() && expr.operands[0] &&
            expr.operands[0]->kind == ExprKind::Identifier) {
            if (navigate_names_.count(expr.operands[0]->text) != 0) {
                rewrite_navigate_call(expr);
            } else if (expr.operands[0]->text == "useEffect" ||
                       expr.operands[0]->text == "useLayoutEffect") {
                drop_navigate_dependency(expr);
            }
        }
    }

    // `navigate` is stable in react-router and lint rules ask for it in a dependency list; here it
    // is a service binding, not a value, so it is dropped from the list. The body still decides
    // the exact-match check of the remaining names.
    void drop_navigate_dependency(Expr& effect) {
        if (effect.operands.size() < 3 || !effect.operands.back() ||
            effect.operands.back()->kind != ExprKind::Array) {
            return;
        }
        auto& elements = effect.operands.back()->operands;
        std::erase_if(elements, [this](const ExprPtr& element) {
            return element && element->kind == ExprKind::Identifier &&
                   navigate_names_.count(element->text) != 0;
        });
    }

    // `navigate(-1)`: a unary minus applied to the literal 1.
    [[nodiscard]] static bool is_minus_one(const Expr& arg) {
        return arg.kind == ExprKind::Unary && arg.text == "-" && arg.operands.size() == 1 &&
               arg.operands[0] && arg.operands[0]->kind == ExprKind::Number &&
               arg.operands[0]->number == 1.0;
    }

    // `{ replace: true }`/`{ replace: false }`; anything else is out of the table. Returns the
    // method name, or empty after reporting.
    [[nodiscard]] std::string_view options_method(const Expr& options) {
        if (options.kind == ExprKind::Object && options.properties.size() == 1 &&
            options.properties[0].key == "replace" && options.properties[0].value &&
            options.properties[0].value->kind == ExprKind::Boolean) {
            return options.properties[0].value->boolean ? "replace" : "push";
        }
        sink_.error(kForm, "`navigate` accepts only the `replace` option, as a boolean literal",
                    options.span,
                    "write `navigate(path, { replace: true })`; state, relative and "
                    "viewTransition have no equivalent");
        return {};
    }

    void rewrite_navigate_call(Expr& call) {
        const std::size_t argc = call.operands.size() - 1;
        std::string_view method = "push";
        bool keep_argument = true;
        if (argc == 0 || argc > 2) {
            sink_.error(kForm, "`navigate` takes a path and an optional options object", call.span,
                        "write `navigate(\"/path\")` or `navigate(-1)`");
            return;
        }
        const Expr& first = *call.operands[1];
        if (is_minus_one(first) && argc == 1) {
            method = "back";
            keep_argument = false;
        } else if (first.kind == ExprKind::Number || first.kind == ExprKind::Unary) {
            sink_.error(kForm, "`navigate(n)` with a number is supported only as `navigate(-1)`",
                        first.span, "use `navigate(-1)`, or navigate to a path");
            return;
        } else if (first.kind == ExprKind::Object) {
            sink_.error(kForm, "`navigate` takes a path string, not a location object", first.span,
                        "write `navigate(\"/orders/\" + id)`");
            return;
        } else if (argc == 2) {
            method = options_method(*call.operands[2]);
            if (method.empty()) {
                return;
            }
        }
        auto member = std::make_unique<Expr>();
        member->kind = ExprKind::Member;
        member->span = call.operands[0]->span;
        member->text = std::string(method);
        member->height = call.operands[0]->height + 1;
        member->operands.push_back(std::move(call.operands[0]));
        ExprPtr path = keep_argument ? std::move(call.operands[1]) : nullptr;
        call.operands.clear();
        call.operands.push_back(std::move(member));
        if (path) {
            call.operands.push_back(std::move(path));
        }
    }

    Module& module_;
    DiagnosticSink& sink_;
    std::string navigate_local_;
    std::set<std::string> navigate_names_;
};

} // namespace

bool is_shim_package(std::string_view specifier) noexcept {
    return shim::is_known_package(specifier);
}

void expand_shims(Module& module, DiagnosticSink& sink) {
    ShimPass(module, sink).run();
}

} // namespace tessera::compiler::jsx
