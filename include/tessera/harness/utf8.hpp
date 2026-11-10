// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::harness {

// Strict UTF-8 well-formedness (Unicode table 3-7): rejects overlong encodings, surrogate code
// points, values above U+10FFFF, bare continuation bytes, and truncated sequences. The protocol
// validates whole lines rather than individual fields, so a `\u` escape and a raw byte sequence
// obey the same rule at the framing edge.
//
// On failure `error_offset` receives the offset of the first byte of the ill-formed sequence.
[[nodiscard]] bool is_valid_utf8(std::string_view text,
                                 std::size_t* error_offset = nullptr) noexcept;

// Appends the UTF-8 encoding of one scalar value. Returns false, appending nothing, for a
// surrogate or an out-of-range value: callers report a typed error instead of writing garbage.
[[nodiscard]] bool append_utf8(std::string& out, char32_t code_point);

} // namespace tessera::harness
