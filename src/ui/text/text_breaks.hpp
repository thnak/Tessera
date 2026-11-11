// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/edit_buffer.hpp"

#include <cstddef>
#include <optional>
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

// Line break opportunities (UAX #14 via ubrk_* when ICU is linked). These are the segments the
// greedy wrap places, plus the legal break points inside a run wider than the constraint.
[[nodiscard]] BreakBoundaries line_boundaries(std::string_view text);

// The same opportunities from an explicit engine. `Icu` is only available when the ICU slice is
// linked (TESSERA_ICU_ENABLED); an ICU-free build documents the request as "use the builtin
// rules", so the wrap parity test needs no conditional code.
[[nodiscard]] BreakBoundaries line_boundaries(std::string_view text, BreakBackend backend);

// Grapheme-cluster (UAX #29 UBRK_CHARACTER) caret movement. Each returns nullopt when the ICU
// slice is not linked or fails to open, so the caller keeps the documented builtin cluster rule.
// `following` is the first boundary after `offset`, `preceding` the last one before it, and `snap`
// is `offset` itself when it is a boundary, else the boundary before it. Offsets are UTF-8 bytes.
[[nodiscard]] std::optional<EditOffset> grapheme_following(std::string_view text,
                                                           EditOffset offset);
[[nodiscard]] std::optional<EditOffset> grapheme_preceding(std::string_view text,
                                                           EditOffset offset);
[[nodiscard]] std::optional<EditOffset> grapheme_snap(std::string_view text, EditOffset offset);

// The builtin rules, callable directly so parity tests and ICU-free builds share one path.
[[nodiscard]] BreakBoundaries builtin_word_boundaries(std::string_view text);
[[nodiscard]] BreakBoundaries builtin_line_boundaries(std::string_view text);

} // namespace tessera::ui::text
