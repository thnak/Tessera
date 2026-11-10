// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/edit_buffer.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

// Word and line break opportunities for the text stack (docs/design/text.md section 6).
//
// Two backends exist:
//   * Icu     - ubrk_* iterators over the statically linked ICU (TESSERA_ICU_REDUCED_DATA=ON).
//   * Builtin - the documented reduced rules that predate ICU; they keep a parity baseline and
//               serve builds without the ICU slice.
// Every difference between the two is enumerated and tested in
// tests/unit/icu/text_breaks_parity_tests.cpp, and the fixture corpus there is the reference.
namespace tessera::ui::text {

enum class BreakBackend { Builtin, Icu };

// Byte offsets into the source text, ascending, always starting at 0 and ending at text.size()
// (for non-empty text). Offsets always land on a UTF-8 code point boundary.
struct BreakBoundaries {
    std::vector<EditOffset> offsets;
    BreakBackend backend = BreakBackend::Builtin;

    [[nodiscard]] bool empty() const {
        return offsets.empty();
    }
};

// The backend this build uses for word_boundaries()/line_boundaries().
[[nodiscard]] BreakBackend active_backend();

// Word break opportunities (UAX #29 word boundaries via ubrk_* when ICU is linked).
[[nodiscard]] BreakBoundaries word_boundaries(std::string_view text);

// Line break opportunities (UAX #14 via ubrk_* when ICU is linked). Used by the greedy line
// wrap to find legal break points inside a run that is wider than the constraint.
[[nodiscard]] BreakBoundaries line_boundaries(std::string_view text);

// The builtin rules, callable directly so parity tests and ICU-free builds share one path.
[[nodiscard]] BreakBoundaries builtin_word_boundaries(std::string_view text);
[[nodiscard]] BreakBoundaries builtin_line_boundaries(std::string_view text);

} // namespace tessera::ui::text
