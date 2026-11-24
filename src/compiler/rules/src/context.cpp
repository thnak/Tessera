// SPDX-License-Identifier: MIT

// `createContext` and `useContext` in the static form ADR-066 item 5 proposes (issue #802):
//
//   type Theme = { accent: string };
//   const ThemeContext = createContext<Theme>({ accent: "blue" });   // module scope
//   const theme = useContext(ThemeContext);                          // in a component body
//
// The analyzer records what each wrote: ContextInfo for the module-scope consts (on
// AnalyzedModule, because the provider tree walk and every consumer read them) and
// ContextUseInfo for the component-body calls. Lowering turns a consumer into an IR environment
// entry that names its context; codegen resolves which ancestor provider's value each instance
// reads — or the context default when no ancestor provides one (the owner's decision for #802).

#include "expr_walk.hpp"
#include "reporter.hpp"
#include "tessera/compiler/rules/analyzed_module.hpp"

#include <optional>
#include <string>

namespace tessera::compiler::rules {

using jsx::DiagnosticSink;
using jsx::Expr;
using jsx::ExprKind;

namespace {

constexpr std::string_view kForm = "TESSERA_REACT_CONTEXT_FORM";

// A `createContext` call's declared type argument (`Theme`), or nullopt: V1 accepts only the
// explicitly typed form, because the IR type has to be written on the environment and an inferred
// `createContext({ accent: "blue" })` gives the IR nothing to write.
std::optional<std::string> declared_type_argument(const Expr& call) {
    if (call.type_args.size() != 1 || call.type_args[0] == nullptr) {
        return std::nullopt;
    }
    const jsx::TypeNode& type = *call.type_args[0];
    // A name is the only accepted spelling: a record alias (`Theme`), or a primitive (`string`).
    // An inline object type has no name to put in the IR.
    if (type.kind != jsx::TypeKind::Name || !type.children.empty() || type.text.empty()) {
        return std::nullopt;
    }
    return type.text;
}

// The IR type of the context value: the written name for a primitive, or a record name the module
// declares as an object type. An alias to an object keeps the alias's own name, which is what a
// record-typed prop or state cell does too (ADR-051: a record's identity is its name).
std::optional<std::string> context_ir_type(const jsx::Module& module, const std::string& written,
                                           DiagnosticSink& sink, jsx::Span span) {
    static constexpr std::string_view kPrimitives[] = {"bool",  "int",    "float",   "string",
                                                       "color", "length", "node_ref"};
    for (const std::string_view primitive : kPrimitives) {
        if (written == primitive) {
            return std::string(primitive);
        }
    }
    for (const jsx::ModuleItem& item : module.items) {
        if (item.kind == jsx::ItemKind::TypeAlias && item.name == written && item.type &&
            item.type->kind == jsx::TypeKind::Object) {
            return written;
        }
    }
    sink.report(kForm, jsx::Severity::Error,
                "`" + written + "` is not a type declared in this module", span,
                "declare `type " + written + " = { ... };` beside the context", std::nullopt);
    return std::nullopt;
}

} // namespace

void check_module_contexts(const jsx::Module& module, AnalyzedModule& out,
                           jsx::DiagnosticSink& sink) {
    for (std::size_t index = 0; index < module.items.size(); ++index) {
        const jsx::ModuleItem& item = module.items[index];
        if (item.kind != jsx::ItemKind::Const || !item.valid) {
            continue;
        }
        const Expr* call = item.init.get();
        if (call == nullptr || call->kind != ExprKind::Call ||
            callee_name(*call) != "createContext") {
            continue;
        }
        const auto reject = [&](const std::string& message, const std::string& note) {
            sink.report(kForm, jsx::Severity::Error, message, item.span, note, std::nullopt);
        };
        if (call->operands.size() != 2 || !call->operands[1]) {
            reject("`createContext` takes exactly one default value",
                   "write `const Ctx = createContext<Type>(default);`");
            continue;
        }
        const std::optional<std::string> written = declared_type_argument(*call);
        if (!written) {
            reject("`createContext` needs its value type as an explicit argument",
                   "write `createContext<Type>(default);` — the IR carries the type, so an "
                   "inferred one has nothing to name it");
            continue;
        }
        const std::optional<std::string> ir_type =
            context_ir_type(module, *written, sink, item.span);
        if (!ir_type) {
            continue; // the type diagnostic is already reported
        }
        ContextInfo info;
        info.item = index;
        info.name = item.name;
        info.type = *written;
        info.ir_type = *ir_type;
        info.default_value = call->operands[1].get();
        info.span = item.span;
        out.contexts.push_back(std::move(info));
    }
}

void check_contexts(Context& ctx) {
    // ADR-066 item 5 runs before the placement rules so that its one symbol exists for the order
    // check, exactly as `check_reducers` does.
    const auto& body = ctx.item.body;
    for (std::size_t index = 0; index < body.size(); ++index) {
        const jsx::Statement& statement = body[index];
        if (!statement.init || callee_name(*statement.init) != "useContext") {
            continue;
        }
        const Expr& call = *statement.init;
        const auto reject = [&](const std::string& message, jsx::Span span,
                                const std::string& note) { ctx.error(kForm, message, span, note); };
        if (statement.kind != jsx::StatementKind::Const || statement.names.size() != 1) {
            reject("`useContext` binds one value: `const theme = useContext(ThemeContext);`",
                   statement.span, "read a field of the bound value where it is used");
            continue;
        }
        if (call.operands.size() != 2 || !call.operands[1] ||
            call.operands[1]->kind != ExprKind::Identifier) {
            reject("the argument of `useContext` must name a module-scope context", call.span,
                   "write `useContext(ThemeContext)` with `const ThemeContext = "
                   "createContext<...>(...)` in this module");
            continue;
        }
        const std::string& context = call.operands[1]->text;
        bool declared = false;
        if (ctx.module_contexts != nullptr) {
            for (const ContextInfo& candidate : *ctx.module_contexts) {
                if (candidate.name == context) {
                    declared = true;
                    break;
                }
            }
        }
        if (!declared) {
            reject("`" + context + "` is not a context declared in this module", call.span,
                   "write `const " + context +
                       " = createContext<...>(...);` at module scope, or use an environment hook "
                       "(`useTheme`, `useBreakpoint`) for an engine value");
            continue;
        }
        ContextUseInfo info;
        info.statement = index;
        info.name = statement.names.front();
        info.context = context;
        info.span = statement.span;
        // The value is an environment local: `theme.accent` resolves as `env::<name>.accent`, the
        // same dotted rule a hook's value reads, so the resolver needs the symbol. This runs
        // before the move into `context_uses`, which empties `info`.
        add_symbol(ctx, info.name, SymbolKind::Environment, statement.span, index);
        ctx.out.context_uses.push_back(std::move(info));
    }
}

} // namespace tessera::compiler::rules
