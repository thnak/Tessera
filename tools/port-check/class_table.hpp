// SPDX-License-Identifier: MIT
//
// The port classes of ADR-058, as data (ADR-058 section 2, issue #546).
//
// **Why a table and not a switch.** The checker and the document have to agree, and the acceptance
// for #546 asks for a test that proves it: `react-compat.md` carries the same four classes in its
// own column, and a rule added to one and not the other would make the document lie about what the
// tool reports. Keeping the classes as rows means the doc test can read this file rather than a
// second hand-written list, and a new dependency is one row.
//
// The four classes are ADR-058's:
//
//   A  source-compatible   the same source compiles unchanged
//   B  shim                a Tessera module provides the same API under the same import
//   C  build-time          the build rewrites it (aliases, assets, `*.module.css`)
//   D  not ported          there is no equivalent; the row names the replacement
//
// A class-D row is never a bare "no": ADR-058 section 1 requires each one to name what to write
// instead, so `replacement` is null only for a class that does not need one.
#pragma once

#include <span>
#include <string_view>

namespace tessera::port_check {

// ADR-058 section 2. `NotPorted` is the only class that blocks a port outright.
enum class PortClass {
    SourceCompatible, // A
    Shim,             // B
    BuildTime,        // C
    NotPorted,        // D
};

// The letter the report and the document print.
[[nodiscard]] char class_letter(PortClass value) noexcept;

// One row: a package name (or `*` for "any package"), the class it falls in, and what a port author
// does about it.
struct ClassRow {
    // The npm package name this row is about, or "*" for a rule that applies to any package that no
    // other row named. Exact matches win over "*", so the wildcard is a fallback and never a
    // shadow.
    std::string_view package;
    PortClass port_class;
    // What to write instead. Empty for classes A and B (nothing to do), required for C and D - the
    // table check rejects a D row without one.
    std::string_view replacement;
    // The ADR-058 or spec rule the row comes from, so the report can cite rather than assert.
    std::string_view source;
};

// The rows the checker uses. Sorted by package for a stable report.
[[nodiscard]] std::span<const ClassRow> class_rows() noexcept;

// The class a package name falls in: its own row when it has one, else the wildcard row.
[[nodiscard]] const ClassRow* find_row(std::string_view package) noexcept;

} // namespace tessera::port_check
