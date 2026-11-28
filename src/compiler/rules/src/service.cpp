// SPDX-License-Identifier: MIT

// `useService<T>()` and the calls on its binding (ADR-050 section 3.1,
// docs/specs/services-manifest- v1.md section 3, issue #177):
//
//   const nav = useService<Navigator>();                 // top level of the component body
//   <Button onClick={() => nav.push("/compose")} />      // a call in a handler or an effect
//
// `check_services` resolves `T` against the project's services manifest and records the use;
// `check_service_calls` then checks every mention of the binding: only calls of a manifest method
// with the declared number of arguments, only inside a function (a plain method result is not
// reactive, so a call while rendering is rejected), and never the service as a value. Lowering
// reads what was recorded, so it needs no manifest of its own.

#include "expr_walk.hpp"
#include "reporter.hpp"
#include "tessera/compiler/services/builtin_services.hpp"

#include <string>

namespace tessera::compiler::rules {

using jsx::Expr;
using jsx::ExprKind;

namespace {

constexpr std::string_view kForm = "TESSERA_REACT_HOOK_FORM";
constexpr std::string_view kUnknown = "TESSERA_REACT_SERVICE_UNKNOWN";
constexpr std::string_view kMethod = "TESSERA_REACT_SERVICE_METHOD";
constexpr std::string_view kRender = "TESSERA_REACT_SERVICE_CALL_IN_RENDER";
constexpr std::string_view kValue = "TESSERA_REACT_SERVICE_VALUE";

// The names a manifest declares, for the "did you mean" note.
std::string declared_names(const services::ServicesManifest& manifest) {
    std::string names;
    for (const services::ManifestService& service : manifest.services) {
        names += (names.empty() ? "" : ", ") + service.name;
    }
    return names.empty() ? "none" : names;
}

std::string method_names(const services::ManifestService& service) {
    std::string names;
    for (const services::ManifestMethod& method : service.methods) {
        names += (names.empty() ? "" : ", ") + method.name;
    }
    return names.empty() ? "none" : names;
}

// The `ServiceUseInfo` of a binding, or null when `name` is not a service binding.
const ServiceUseInfo* find_use(const ComponentAnalysis& analysis, std::string_view name) {
    for (const ServiceUseInfo& use : analysis.service_uses) {
        if (use.name == name) {
            return &use;
        }
    }
    return nullptr;
}

// The receiver of a `svc.method(...)` call when it is a service binding, else null.
const ServiceUseInfo* service_receiver(const ComponentAnalysis& analysis, const Expr& call) {
    if (call.kind != ExprKind::Call || call.operands.empty() || !call.operands[0]) {
        return nullptr;
    }
    const Expr& callee = *call.operands[0];
    if (callee.kind != ExprKind::Member || callee.operands.empty() || !callee.operands[0] ||
        callee.operands[0]->kind != ExprKind::Identifier) {
        return nullptr;
    }
    const Symbol* symbol = analysis.find(callee.operands[0]->text);
    if (symbol == nullptr || symbol->kind != SymbolKind::Service) {
        return nullptr;
    }
    return find_use(analysis, symbol->name);
}

// A literal argument whose kind cannot be the manifest's parameter type. Anything that is not a
// plain literal (a prop, a state value, an event field) is checked later, where its type is known.
bool literal_mismatch(const Expr& argument, std::string_view type) {
    switch (argument.kind) {
    case ExprKind::String:
    case ExprKind::Template:
        return type != "string";
    case ExprKind::Boolean:
        return type != "bool";
    case ExprKind::Number: {
        const bool integral = argument.text.find_first_of(".eE") == std::string::npos;
        return !(type == "float" || (type == "int" && integral));
    }
    default:
        return false;
    }
}

class CallScanner {
public:
    explicit CallScanner(Context& ctx)
        : ctx_(ctx) {}

    void scan(const Expr& expr, bool in_function) {
        if (expr.kind == ExprKind::Arrow) {
            scan_children(expr, true);
            return;
        }
        if (is_resource_call(expr)) {
            // The first argument names a service method as a value (`useResource(api.load, ...)`);
            // resource.cpp checks it. Only the key and the options are scanned here.
            for (std::size_t i = 2; i < expr.operands.size(); ++i) {
                scan(*expr.operands[i], in_function);
            }
            return;
        }
        if (const ServiceUseInfo* use = service_receiver(ctx_.out, expr); use != nullptr) {
            check_call(expr, *use, in_function);
            // The receiver is not a value use; only the arguments are scanned further.
            for (std::size_t i = 1; i < expr.operands.size(); ++i) {
                scan(*expr.operands[i], in_function);
            }
            return;
        }
        if (expr.kind == ExprKind::Identifier) {
            const Symbol* symbol = ctx_.out.find(expr.text);
            if (symbol != nullptr && symbol->kind == SymbolKind::Service) {
                ctx_.error(kValue,
                           "service `" + expr.text + "` is only used by calling one of its methods",
                           expr.span, "write `" + expr.text + ".method(...)` in a handler");
            }
            return;
        }
        scan_children(expr, in_function);
    }

    void scan_jsx(const jsx::JsxNode& node) {
        auto visit = [this](const Expr& child) { scan(child, false); };
        for_each_jsx_expr(node, visit);
    }

private:
    void scan_children(const Expr& expr, bool in_function) {
        auto visit = [this, in_function](const Expr& child) { scan(child, in_function); };
        for_each_child(expr, visit);
    }

    void check_call(const Expr& call, const ServiceUseInfo& use, bool in_function) {
        const Expr& callee = *call.operands[0];
        const std::string text = use.name + "." + callee.text;
        if (!in_function) {
            ctx_.error(kRender,
                       "`" + text +
                           "(...)` is called while rendering; a service call is a side "
                           "effect and may only run in a handler or an effect",
                       call.span, "move the call into an event handler or `useEffect`");
            return;
        }
        const services::ManifestMethod* method = use.decl->find_method(callee.text);
        if (method == nullptr) {
            ctx_.error(kMethod,
                       "service `" + use.service + "` lists no method `" + callee.text + "`",
                       callee.span, "the manifest lists: " + method_names(*use.decl));
            return;
        }
        const std::size_t given = call.operands.size() - 1;
        if (given != method->params.size()) {
            ctx_.error(kMethod,
                       "`" + text + "` takes " + std::to_string(method->params.size()) +
                           " argument(s), not " + std::to_string(given),
                       call.span,
                       "the manifest declares " + method->name + "(" + params_text(*method) + ")");
            return;
        }
        for (std::size_t i = 0; i < given; ++i) {
            const Expr& argument = *call.operands[i + 1];
            const services::ManifestParam& param = method->params[i];
            if (literal_mismatch(argument, param.type)) {
                ctx_.error(kMethod,
                           "argument " + std::to_string(i + 1) + " of `" + text + "` (`" +
                               param.name + "`) is declared `" + param.type +
                               "`, which this literal is not",
                           argument.span);
            }
        }
    }

    static std::string params_text(const services::ManifestMethod& method) {
        std::string text;
        for (const services::ManifestParam& param : method.params) {
            text += (text.empty() ? "" : ", ") + param.name + ": " + param.type;
        }
        return text;
    }

    Context& ctx_;
};

// Records one accepted `useService` binding.
void add_service_use(Context& ctx, const jsx::Statement& statement, std::size_t index,
                     const std::string& service, const services::ManifestService* decl) {
    ServiceUseInfo info;
    info.statement = index;
    info.name = statement.names.front();
    info.service = service;
    info.decl = decl;
    info.span = statement.span;
    // The binding is a hook symbol so a statement that uses it before this one is an order
    // error, and it has no IR expression: its only use is a method call.
    add_symbol(ctx, info.name, SymbolKind::Service, statement.span, index);
    ctx.out.service_uses.push_back(std::move(info));
}

} // namespace

void check_services(Context& ctx) {
    const auto& body = ctx.item.body;
    for (std::size_t index = 0; index < body.size(); ++index) {
        const jsx::Statement& statement = body[index];
        if (!statement.init || callee_name(*statement.init) != "useService") {
            continue;
        }
        const Expr& call = *statement.init;
        const std::string_view shape = "write `const nav = useService<Navigator>();`";
        const bool form_ok = statement.kind == jsx::StatementKind::Const &&
                             !statement.array_pattern && statement.names.size() == 1 &&
                             call.operands.size() == 1 && call.type_args.size() == 1 &&
                             call.type_args[0] != nullptr;
        if (!form_ok) {
            ctx.error(kForm, "`useService` is not written in its accepted form", statement.span,
                      std::string(shape));
            continue;
        }
        const jsx::TypeNode& type = *call.type_args[0];
        if (type.kind != jsx::TypeKind::Name || !type.children.empty() || type.text.empty()) {
            ctx.error(kUnknown,
                      "`useService` takes the name of a service interface from `ui/services.json`, "
                      "not a concrete class, a string or a variable",
                      statement.span, std::string(shape));
            continue;
        }
        // The shims' interfaces are always visible; a manifest entry of the same name wins.
        const services::ManifestService* builtin = services::find_builtin_service(type.text);
        const services::ManifestService* listed =
            ctx.services == nullptr ? nullptr : ctx.services->find_name(type.text);
        if (listed == nullptr && builtin != nullptr) {
            add_service_use(ctx, statement, index, type.text, builtin);
            continue;
        }
        if (ctx.services == nullptr) {
            ctx.error(
                kUnknown,
                "`" + type.text +
                    "` is not a service interface: this project has no "
                    "`ui/services.json`",
                statement.span,
                "declare the interface in `ui/services.json` (docs/specs/services-manifest-v1.md)");
            continue;
        }
        const services::ManifestService* decl = listed;
        if (decl == nullptr) {
            ctx.error(kUnknown,
                      "`" + type.text + "` is not a service interface listed in `ui/services.json`",
                      statement.span, "the manifest lists: " + declared_names(*ctx.services));
            continue;
        }
        add_service_use(ctx, statement, index, type.text, decl);
    }
}

void check_service_calls(Context& ctx) {
    if (ctx.out.service_uses.empty()) {
        return;
    }
    CallScanner scanner(ctx);
    const auto& body = ctx.item.body;
    for (std::size_t index = 0; index < body.size(); ++index) {
        const jsx::Statement& statement = body[index];
        if (!statement.init || callee_name(*statement.init) == "useService") {
            continue;
        }
        scanner.scan(*statement.init, false);
    }
    if (ctx.item.returned) {
        scanner.scan_jsx(*ctx.item.returned);
    }
}

bool is_service_call(const Context& ctx, const Expr& expr) {
    const ServiceUseInfo* use = service_receiver(ctx.out, expr);
    if (use == nullptr) {
        return false;
    }
    return use->decl->find_method(expr.operands[0]->text) != nullptr;
}

} // namespace tessera::compiler::rules
