// SPDX-License-Identifier: MIT

#include "file_indexer.hpp"
#include "tessera/compiler/jsx/parser.hpp"

#include <algorithm>
#include <cctype>

namespace tessera::compiler::analysis {

namespace {

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += '/';
        }
        out += part;
    }
    return out;
}

bool is_state_hook(const jsx::Expr* init) {
    if (init == nullptr || init->kind != jsx::ExprKind::Call || init->operands.empty()) {
        return false;
    }
    const jsx::Expr& callee = *init->operands.front();
    return callee.kind == jsx::ExprKind::Identifier &&
           (callee.text == "useState" || callee.text == "useReducer");
}

} // namespace

FileIndex index_file(FileId file, std::string_view text, bool is_source) {
    FileIndex empty;
    empty.file = file;
    if (!is_source) {
        return empty;
    }
    const jsx::ModuleParseResult parsed = jsx::parse_module(text);
    const Utf16Map map(text);
    std::vector<Diagnostic> diagnostics;
    for (const jsx::Diagnostic& found : parsed.diagnostics) {
        Diagnostic out;
        out.span =
            Span{file, map.to_utf16(found.span.span.offset), map.to_utf16(found.span.span.end())};
        out.severity = found.severity == jsx::Severity::Error ? DiagnosticSeverity::Error
                                                              : DiagnosticSeverity::Warning;
        out.code = std::string(found.code);
        out.message = found.message;
        if (found.related) {
            out.related = Span{file, map.to_utf16(found.related->span.span.offset),
                               map.to_utf16(found.related->span.span.end())};
            out.related_message = found.related->message;
        }
        diagnostics.push_back(std::move(out));
    }
    // A file the parser cannot read at all keeps its diagnostics and no symbols.
    FileIndex out =
        parsed.root == nullptr ? std::move(empty) : FileIndexer(file, text).run(*parsed.root);
    out.diagnostics = std::move(diagnostics);
    return out;
}

FileIndexer::FileIndexer(FileId file, std::string_view source)
    : file_(file),
      source_(source),
      map_(source) {
    out_.file = file;
}

FileIndex FileIndexer::run(const jsx::Module& module) {
    scopes_.emplace_back();
    declare_items(module);
    for (const jsx::ModuleItem& item : module.items) {
        index_item(item);
    }
    // Symbols and references in source order so every consumer sees one deterministic sequence.
    std::ranges::sort(out_.symbols, [](const Symbol& a, const Symbol& b) {
        return a.span.from != b.span.from ? a.span.from < b.span.from : a.id < b.id;
    });
    std::ranges::stable_sort(out_.references, [](const Reference& a, const Reference& b) {
        return a.span.from < b.span.from;
    });
    out_.indexed = true;
    return std::move(out_);
}

Span FileIndexer::span_of(jsx::Span bytes) const {
    return span_at(bytes.offset, bytes.length);
}

Span FileIndexer::span_at(std::uint32_t byte, std::size_t length) const {
    return Span{file_, map_.to_utf16(byte),
                map_.to_utf16(byte + static_cast<std::uint32_t>(length))};
}

std::optional<std::uint32_t> FileIndexer::find_ident(std::string_view name, std::uint32_t from,
                                                     std::uint32_t to) const {
    to = std::min<std::uint32_t>(to, static_cast<std::uint32_t>(source_.size()));
    std::size_t pos = from;
    while (!name.empty() && pos < to) {
        pos = source_.find(name, pos);
        if (pos == std::string_view::npos || pos + name.size() > to) {
            return std::nullopt;
        }
        const bool left = pos == 0 || !is_ident_char(source_[pos - 1]);
        const bool right =
            pos + name.size() >= source_.size() || !is_ident_char(source_[pos + name.size()]);
        if (left && right) {
            return static_cast<std::uint32_t>(pos);
        }
        ++pos;
    }
    return std::nullopt;
}

Span FileIndexer::next_name(std::string_view name, jsx::Span within, std::uint32_t* cursor) const {
    const std::uint32_t start = std::max(*cursor, within.offset);
    if (const auto found = find_ident(name, start, within.end())) {
        *cursor = *found + static_cast<std::uint32_t>(name.size());
        return span_at(*found, name.size());
    }
    return span_at(within.offset, 0);
}

void FileIndexer::push_scope(std::string label) {
    path_.push_back(std::move(label));
    scopes_.emplace_back();
}

void FileIndexer::pop_scope() {
    scopes_.pop_back();
    path_.pop_back();
}

std::string FileIndexer::anonymous_label() {
    return "~" + std::to_string(++anonymous_counts_[join(path_)]);
}

std::size_t FileIndexer::declare(const std::string& name, SymbolKind kind, Span span, bool scoped) {
    std::string id = "f" + std::to_string(file_) + ":" + join(path_);
    id += path_.empty() ? "" : "/";
    id += name;
    if (const std::uint32_t seen = id_counts_[id]++; seen > 0) {
        id += "#" + std::to_string(seen + 1);
    }
    Symbol symbol;
    symbol.id = std::move(id);
    symbol.kind = kind;
    symbol.name = name;
    symbol.span = span;
    symbol.parent = parents_.empty() ? std::string() : parents_.back();
    out_.symbols.push_back(std::move(symbol));
    const std::size_t index = out_.symbols.size() - 1;
    if (scoped) {
        scopes_.back()[name] = index;
    }
    return index;
}

std::optional<std::size_t> FileIndexer::lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (const auto found = it->find(name); found != it->end()) {
            return found->second;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> FileIndexer::lookup_type(const std::string& name) const {
    const auto found = scopes_.front().find(name);
    if (found == scopes_.front().end()) {
        return std::nullopt;
    }
    const SymbolKind kind = out_.symbols[found->second].kind;
    if (kind == SymbolKind::TypeAlias || kind == SymbolKind::Import) {
        return found->second;
    }
    return std::nullopt;
}

void FileIndexer::add_reference(Span span, ReferenceKind kind, std::string name,
                                Resolution resolution, std::string via) {
    out_.references.push_back(
        Reference{span, kind, std::move(name), std::move(resolution), std::move(via)});
}

void FileIndexer::reference_value(std::string name, Span span) {
    if (const auto found = lookup(name)) {
        add_reference(span, ReferenceKind::Value, name, Resolution{out_.symbols[*found].id, {}});
        return;
    }
    static constexpr std::string_view kGlobals[] = {
        "undefined", "NaN",     "Infinity", "Math",   "JSON", "console",  "Number",
        "String",    "Boolean", "Array",    "Object", "Date", "parseInt", "parseFloat"};
    const bool hook = name.size() > 3 && name.starts_with("use") &&
                      std::isupper(static_cast<unsigned char>(name[3])) != 0;
    const bool global = std::ranges::find(kGlobals, name) != std::end(kGlobals);
    add_reference(span, ReferenceKind::Value, std::move(name),
                  Resolution{{}, hook || global ? Unresolved::Builtin : Unresolved::Unbound});
}

void FileIndexer::declare_items(const jsx::Module& module) {
    for (const jsx::ModuleItem& item : module.items) {
        switch (item.kind) {
        case jsx::ItemKind::Import: {
            if (item.ignored_react_import) {
                break;
            }
            const jsx::Span all = item.span;
            if (!item.css_module_binding.empty()) {
                std::uint32_t cursor = all.offset;
                const Span span = next_name(item.css_module_binding, all, &cursor);
                Symbol* symbol =
                    &out_.symbols[declare(item.css_module_binding, SymbolKind::CssModule, span)];
                symbol->import_source = item.source;
            }
            for (const jsx::ImportName& imported : item.names) {
                Span span = span_of(imported.span);
                if (!imported.local_name.empty()) {
                    std::uint32_t cursor = imported.span.end();
                    span = next_name(imported.local_name, all, &cursor);
                }
                const std::size_t index =
                    declare(std::string(imported.local()), SymbolKind::Import, span);
                out_.symbols[index].import_source = item.source;
                out_.symbols[index].import_name = imported.name;
            }
            break;
        }
        case jsx::ItemKind::TypeAlias:
        case jsx::ItemKind::Const:
        case jsx::ItemKind::Component:
        case jsx::ItemKind::Hook:
        case jsx::ItemKind::Helper: {
            if (item.name.empty()) {
                break;
            }
            SymbolKind kind = SymbolKind::Const;
            switch (item.kind) {
            case jsx::ItemKind::TypeAlias:
                kind = SymbolKind::TypeAlias;
                break;
            case jsx::ItemKind::Component:
                kind = SymbolKind::Component;
                break;
            case jsx::ItemKind::Hook:
                kind = SymbolKind::Hook;
                break;
            case jsx::ItemKind::Helper:
                kind = SymbolKind::Helper;
                break;
            default:
                break;
            }
            const std::size_t index = declare(item.name, kind, span_of(item.name_span));
            out_.symbols[index].exported = item.exported;
            item_symbols_[&item] = index;
            break;
        }
        default:
            break;
        }
    }
}

void FileIndexer::index_item(const jsx::ModuleItem& item) {
    switch (item.kind) {
    case jsx::ItemKind::ReExport:
        index_re_export(item);
        break;
    case jsx::ItemKind::TypeAlias:
        walk_type(item.type.get());
        break;
    case jsx::ItemKind::Const:
        walk_type(item.type.get());
        walk(item.init.get());
        break;
    case jsx::ItemKind::Component:
        index_component(item);
        break;
    case jsx::ItemKind::Hook:
        index_hook(item);
        break;
    case jsx::ItemKind::Helper: {
        // The body of a helper is skipped by the parser, so only its signature is indexed.
        const auto owner = item_symbols_.find(&item);
        push_scope(item.name);
        parents_.push_back(owner != item_symbols_.end() ? out_.symbols[owner->second].id
                                                        : std::string());
        for (const jsx::HelperParam& param : item.helper_params) {
            std::uint32_t cursor = param.span.offset;
            declare(param.name, SymbolKind::Param, next_name(param.name, param.span, &cursor));
            walk_type(param.type.get());
        }
        parents_.pop_back();
        pop_scope();
        break;
    }
    default:
        break;
    }
}

void FileIndexer::index_re_export(const jsx::ModuleItem& item) {
    for (const jsx::ImportName& entry : item.names) {
        const Span span = span_of(entry.span);
        const std::string exported = std::string(entry.local());
        out_.re_exports.push_back(ReExport{exported, entry.name, item.source});
        if (!item.source.empty()) {
            add_reference(span, ReferenceKind::Export, entry.name,
                          Resolution{{}, Unresolved::ImportUnresolved}, item.source);
            continue;
        }
        const auto local = lookup(entry.name);
        if (local && entry.local_name.empty()) {
            out_.symbols[*local].exported = true;
        }
        add_reference(span, ReferenceKind::Export, entry.name,
                      local ? Resolution{out_.symbols[*local].id, {}}
                            : Resolution{{}, Unresolved::Unbound});
    }
}

void FileIndexer::index_component(const jsx::ModuleItem& item) {
    const auto owner = item_symbols_.find(&item);
    push_scope(item.name);
    parents_.push_back(owner != item_symbols_.end() ? out_.symbols[owner->second].id
                                                    : std::string());
    for (const jsx::PropBinding& binding : item.props_bindings) {
        std::uint32_t cursor = binding.span.offset;
        declare(binding.name, SymbolKind::Prop, next_name(binding.name, binding.span, &cursor));
    }
    if (!item.props_name.empty()) {
        std::uint32_t cursor = item.name_span.end();
        declare(item.props_name, SymbolKind::PropsParam,
                next_name(item.props_name, item.span, &cursor));
    }
    walk_type(item.props_type.get());
    walk_type(item.type.get());
    for (const jsx::PropBinding& binding : item.props_bindings) {
        walk(binding.default_value.get());
    }
    for (const jsx::Statement& statement : item.body) {
        index_statement(statement);
    }
    walk_jsx(item.returned.get());
    parents_.pop_back();
    pop_scope();
}

void FileIndexer::index_hook(const jsx::ModuleItem& item) {
    const auto owner = item_symbols_.find(&item);
    push_scope(item.name);
    parents_.push_back(owner != item_symbols_.end() ? out_.symbols[owner->second].id
                                                    : std::string());
    for (const jsx::CustomHookParam& param : item.hook_params) {
        std::uint32_t cursor = param.span.offset;
        declare(param.name, SymbolKind::Param, next_name(param.name, param.span, &cursor));
        walk_type(param.type.get());
        walk(param.default_value.get());
    }
    for (const jsx::Statement& statement : item.body) {
        index_statement(statement);
    }
    walk(item.hook_return.get());
    parents_.pop_back();
    pop_scope();
}

void FileIndexer::index_statement(const jsx::Statement& statement) {
    walk_type(statement.type.get());
    walk(statement.init.get());
    for (const jsx::DependencyName& dep : statement.memo_deps) {
        reference_value(dep.name, span_of(dep.span));
    }
    if (statement.kind != jsx::StatementKind::Const) {
        return;
    }
    if (!statement.object_pattern.empty()) {
        for (const jsx::PatternBinding& binding : statement.object_pattern) {
            std::uint32_t cursor = binding.span.offset;
            declare(binding.local, SymbolKind::Local,
                    next_name(binding.local, binding.span, &cursor));
        }
        return;
    }
    std::uint32_t cursor = statement.span.offset;
    if (const auto keyword = find_ident("const", cursor, statement.span.end())) {
        cursor = *keyword + 5;
    }
    const bool state = statement.array_pattern && is_state_hook(statement.init.get());
    const bool handler = !statement.array_pattern && statement.init != nullptr &&
                         statement.init->kind == jsx::ExprKind::Arrow;
    for (std::size_t i = 0; i < statement.names.size(); ++i) {
        SymbolKind kind = SymbolKind::Local;
        if (state && i == 0) {
            kind = SymbolKind::State;
        } else if (state && i == 1) {
            kind = SymbolKind::Setter;
        } else if (handler) {
            kind = SymbolKind::Handler;
        }
        declare(statement.names[i], kind, next_name(statement.names[i], statement.span, &cursor));
    }
}

} // namespace tessera::compiler::analysis
