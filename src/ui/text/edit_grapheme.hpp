// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/edit_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Internal UTF-8, grapheme-cluster, word, and line helpers for the editing buffer. Not installed.
// The cluster rules are the documented simplified set of docs/specs/text-editing-v1.md section 12.
namespace tessera::ui::text::edit_detail {

// Standard UTF-8 validation: no overlong forms, no surrogates, no code points above U+10FFFF.
[[nodiscard]] bool valid_utf8(std::string_view text);

// Collapses CRLF and lone CR to LF. The buffer never stores CR.
[[nodiscard]] std::string normalize_newlines(std::string_view text);

// Byte length of the code point whose lead byte is `lead` (1 for an invalid/continuation lead).
[[nodiscard]] std::size_t codepoint_byte_length(unsigned char lead);

// Start of the next code point (or `text.size()`); one byte forward for invalid input.
[[nodiscard]] EditOffset next_codepoint(std::string_view text, EditOffset offset);
// Start of the previous code point (or 0).
[[nodiscard]] EditOffset prev_codepoint(std::string_view text, EditOffset offset);
// The code point starting at `offset`; 0xFFFD for invalid input.
[[nodiscard]] std::uint32_t codepoint_at(std::string_view text, EditOffset offset);

// Cluster boundary after `offset` (>= offset + one code point). A cluster is one base code point
// plus its Extend code points, a ZWJ sequence, or a regional-indicator pair; CRLF is one cluster.
[[nodiscard]] EditOffset next_cluster(std::string_view text, EditOffset offset);
// Cluster boundary before `offset`.
[[nodiscard]] EditOffset prev_cluster(std::string_view text, EditOffset offset);
// Clamps to [0, size] and snaps down to the start of the containing cluster.
[[nodiscard]] EditOffset snap_to_cluster(std::string_view text, EditOffset offset);

// Word boundaries. With the ICU slice compiled in, `word_left`/`word_right` walk UAX #29 word
// boundaries from ui/text/text_breaks.hpp; without it they walk the documented builtin rule
// (ASCII alphanumerics and '_' are word code points, every non-ASCII code point is a word code
// point). `word_left` moves to the start of the previous word, `word_right` to the end of the
// current or next word. The parity differences are enumerated in tests/unit/icu/.
[[nodiscard]] EditOffset word_left(std::string_view text, EditOffset offset);
[[nodiscard]] EditOffset word_right(std::string_view text, EditOffset offset);

// The word-code-point rule both backends share for navigation decisions.
[[nodiscard]] bool is_word_codepoint(std::uint32_t cp);

// Paragraph boundaries at LF. `line_start` is the byte after the previous LF (0 on the first
// line); `line_end` is the next LF (size on the last line). These stay LF-based on purpose: they
// are paragraph/caret helpers, not UAX #14 opportunities (ui/text/text_breaks.hpp owns those).
[[nodiscard]] EditOffset line_start(std::string_view text, EditOffset offset);
[[nodiscard]] EditOffset line_end(std::string_view text, EditOffset offset);

[[nodiscard]] std::uint32_t count_codepoints(std::string_view text, EditOffset end);
[[nodiscard]] std::uint32_t count_graphemes(std::string_view text, EditOffset end);

} // namespace tessera::ui::text::edit_detail
