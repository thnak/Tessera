// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/analysis/symbol_index.hpp"
#include "tessera/compiler/jsx/ast.hpp"
#include "tessera/compiler/jsx/module_ast.hpp"
#include "tessera/compiler/jsx/source_map.hpp"
#include "utf16_map.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tessera::compiler::analysis {

// Parses one file and indexes its declarations and references. A pure function of the text: it
// knows nothing of other files, so the project pass resolves imports afterwards.
FileIndex index_file(FileId file, std::string_view text, bool is_source);

// The walk over one parsed module. Internal to the library; split over file_indexer*.cpp.
class FileIndexer {
public:
    FileIndexer(FileId file, std::string_view source);

    FileIndex run(const jsx::Module& module);

private:
    using Scope = std::unordered_map<std::string, std::size_t>;

    // Declarations (file_indexer.cpp)
    void declare_items(const jsx::Module& module);
    void index_item(const jsx::ModuleItem& item);
    void index_component(const jsx::ModuleItem& item);
    void index_hook(const jsx::ModuleItem& item);
    void index_statement(const jsx::Statement& statement);
    void index_re_export(const jsx::ModuleItem& item);
    std::size_t declare(const std::string& name, SymbolKind kind, Span span, bool scoped = true);
    void push_scope(std::string label);
    void pop_scope();
    std::string anonymous_label();

    // Expressions, JSX and types (file_indexer_expr.cpp)
    void walk(const jsx::Expr* expr);
    void walk_arrow(const jsx::Expr& arrow);
    void walk_member(const jsx::Expr& member);
    void walk_jsx(const jsx::JsxNode* node);
    void walk_type(const jsx::TypeNode* type);

    // Helpers
    [[nodiscard]] Span span_of(jsx::Span bytes) const;
    [[nodiscard]] Span span_at(std::uint32_t byte, std::size_t length) const;
    [[nodiscard]] std::optional<std::uint32_t> find_ident(std::string_view name, std::uint32_t from,
                                                          std::uint32_t to) const;
    // The span of `name` found at or after `*cursor` inside `within`; advances the cursor.
    [[nodiscard]] Span next_name(std::string_view name, jsx::Span within,
                                 std::uint32_t* cursor) const;
    [[nodiscard]] std::optional<std::size_t> lookup(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> lookup_type(const std::string& name) const;
    void add_reference(Span span, ReferenceKind kind, std::string name, Resolution resolution,
                       std::string via = {});
    void reference_value(std::string name, Span span);

    FileId file_;
    std::string_view source_;
    Utf16Map map_;
    FileIndex out_;
    std::vector<Scope> scopes_;
    std::vector<std::string> path_;
    std::vector<std::string> parents_;
    std::unordered_map<std::string, std::uint32_t> id_counts_;
    std::unordered_map<std::string, std::uint32_t> anonymous_counts_;
    std::unordered_map<const jsx::ModuleItem*, std::size_t> item_symbols_;
};

} // namespace tessera::compiler::analysis
