// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tessera::compiler::jsx {

// Length in bytes of the well-formed UTF-8 sequence that starts at `index`, or 0 when the bytes
// there are not well-formed (stray continuation, truncated, overlong, surrogate, > U+10FFFF).
std::size_t utf8_sequence_length(std::string_view text, std::size_t index);

// Appends `code_point` as UTF-8. Callers pass scalar values only (no surrogates, <= 0x10FFFF).
void append_utf8(std::string& out, char32_t code_point);

} // namespace tessera::compiler::jsx
