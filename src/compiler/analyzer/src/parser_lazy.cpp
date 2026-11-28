// SPDX-License-Identifier: MIT

// `lazy(() => import("./Name"))` (ADR-067 item 6, docs/specs/pending-ui-v1.md section 7). Split out
// of parser_module.cpp, which sits at the file-size limit; the subject is one module-level form.

#include "parser_internal.hpp"

namespace tessera::compiler::jsx {
namespace {

// A component name starts uppercase (docs/specs/jsx-subset-v1.md section 6).
bool is_component_name(std::string_view name) {
    return !name.empty() && name[0] >= 'A' && name[0] <= 'Z';
}

} // namespace

// `lazy(() => import("./Name"))` after `const Name =`. The call form is exactly that: one
// parameterless arrow with one `import("...")`. Anything else (a `.then`, a named export, a
// computed specifier) is `TESSERA_REACT_UNSUPPORTED_API`, because the front end resolves the import
// at compile time and cannot follow code it does not see.
bool Parser::parse_lazy_import(ModuleItem& item) {
    const Span lazy_span = cur_.span;
    const std::string binding = item.name;
    const Span binding_span = item.name_span;
    const auto reject = [&](Span span) {
        fail("TESSERA_REACT_UNSUPPORTED_API",
             "`lazy` is supported only as `lazy(() => import(\"./Name\"))`, bound to the "
             "component's "
             "own name",
             span, "write `const Name = lazy(() => import(\"./Name\"));` or import the component");
        return false;
    };
    if (!is_component_name(binding) || item.type) {
        return reject(lazy_span);
    }
    advance(LexMode::Code); // `lazy`
    if (!at_punct("(")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!at_punct("(")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!at_punct(")")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!at_punct("=>")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!cur_.is_keyword("import")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!at_punct("(")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (cur_.kind != TokenKind::String) {
        return reject(cur_.span);
    }
    item.kind = ItemKind::Import;
    item.source = cur_.value;
    const Span source_span = cur_.span;
    advance(LexMode::Code);
    if (!at_punct(")")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    if (!at_punct(")")) {
        return reject(cur_.span);
    }
    advance(LexMode::Code);
    ImportName name;
    name.name = binding;
    name.span = binding_span;
    item.names.push_back(std::move(name));
    item.lazy_import = true;
    check_module_source(item, source_span);
    return expect_semicolon();
}

} // namespace tessera::compiler::jsx
