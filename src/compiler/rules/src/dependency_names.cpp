// SPDX-License-Identifier: MIT

// The reactive-name resolution behind both dependency rules: an effect's exact list
// (`effect_deps.cpp`) and a memo/callback's inferred set (`memo_deps.cpp`, issue #829). Kept in one
// translation unit so the two rules cannot drift on what counts as a reactive input.

#include "dependency_names.hpp"
#include "expr_walk.hpp"

#include <algorithm>

namespace tessera::compiler::rules {
namespace {

bool contains(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

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

std::vector<std::string> required_names(const Context& ctx, const jsx::Expr& body) {
    ReadOptions options;
    options.props_name = ctx.item.props_name;
    std::vector<std::string> required;
    std::vector<std::string> visiting;
    for (const auto& read : collect_reads(body, options)) {
        add_required(ctx, read.name, required, visiting);
    }
    return required;
}

} // namespace tessera::compiler::rules
