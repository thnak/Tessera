// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Symbol and span index data model (issue #570, ADR-060, docs/specs/studio-services-v1.md section
// 10). Plain data only: the same structs back the native CLI/MCP hosts and the Wasm analysis
// worker.
namespace tessera::compiler::analysis {

// A file id is assigned by the page for the open project; a path is never a key (spec section 2).
using FileId = std::uint32_t;

// Half-open range `[from, to)` of UTF-16 code units of the file text (spec section 2). The file
// text is the decoded text without a leading byte order mark, which is what a JavaScript string
// and CodeMirror hold.
struct Span {
    FileId file = 0;
    std::uint32_t from = 0;
    std::uint32_t to = 0;

    friend bool operator==(const Span&, const Span&) = default;
};

enum class SymbolKind : std::uint8_t {
    Component,
    Hook,
    Helper,
    TypeAlias,
    Const,     // a module-scope `const`
    Import,    // the local name an import introduces
    CssModule, // `import styles from "./X.module.css"`
    Prop,      // a destructured props binding of a component
    PropsParam,
    State,   // the value of `const [x, setX] = useState(...)`
    Setter,  // the setter or dispatch of the same pattern
    Handler, // a `const f = (...) => ...` of a component or hook body
    Local,
    Param,
};

// Why a reference has no declaration in the index. A reference is never silently dropped: it
// resolves, or it carries one of these.
enum class Unresolved : std::uint8_t {
    None,
    Unbound,          // no declaration in scope (a library component, a typo, a global)
    Builtin,          // a builtin type, hook or global the language itself provides
    Intrinsic,        // a lowercase JSX tag and its attributes
    External,         // declared outside the indexed text (a CSS class in the .css file)
    NeedsType,        // a member access: resolving it needs the type of the receiver
    ImportUnresolved, // the import's file or exported name is not in the project snapshot
};

enum class ReferenceKind : std::uint8_t {
    Value,     // an identifier in an expression
    Type,      // a type name
    Tag,       // a JSX element name
    Attribute, // a JSX attribute name (a prop of a component tag)
    CssClass,  // `styles.card` on a CSS module binding
    Member,    // the property of `a.b`
    Export,    // a name in `export { A }` or `export { A } from "./x"`
};

// The outcome of resolving one reference.
struct Resolution {
    std::string symbol; // the declaration's id; empty when unresolved
    Unresolved reason = Unresolved::None;

    [[nodiscard]] bool resolved() const {
        return !symbol.empty();
    }
    friend bool operator==(const Resolution&, const Resolution&) = default;
};

struct Symbol {
    // Stable across edits that do not rename or move the declaration: `f<file>:` plus the scope
    // path
    // (`Card/count`, `Card/~1/e` for the first anonymous function of `Card`), with `#n` appended to
    // the n-th duplicate. It never contains an offset.
    std::string id;
    SymbolKind kind = SymbolKind::Local;
    std::string name;
    Span span;          // the declared name
    std::string parent; // the enclosing component or hook; empty at module scope
    bool exported = false;
    // Imports and CSS modules: what the binding names, and (once the project resolves it) the
    // declaration it stands for, or why not.
    std::string import_source;
    std::string import_name;
    Resolution target;

    friend bool operator==(const Symbol&, const Symbol&) = default;
};

struct Reference {
    Span span;
    ReferenceKind kind = ReferenceKind::Value;
    std::string name;
    Resolution resolution;
    // Attribute: the symbol of the tag. CssClass: the CSS module binding. Export with a source:
    // the specifier. Consumed by the project pass.
    std::string via;

    friend bool operator==(const Reference&, const Reference&) = default;
};

// An `export { name as exported } from "source"` entry, kept so the project can follow it.
struct ReExport {
    std::string exported;
    std::string name;
    std::string source;

    friend bool operator==(const ReExport&, const ReExport&) = default;
};

// The index of one file: symbols in source order, references in source order.
struct FileIndex {
    FileId file = 0;
    bool indexed = false; // false for a file the parser could not read, or one that is not TSX/TS
    std::vector<Symbol> symbols;
    std::vector<Reference> references;
    std::vector<ReExport> re_exports;

    friend bool operator==(const FileIndex&, const FileIndex&) = default;
};

} // namespace tessera::compiler::analysis
