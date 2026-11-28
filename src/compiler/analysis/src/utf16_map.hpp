// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace tessera::compiler::analysis {

// Converts between UTF-8 byte offsets (what the parser reports) and UTF-16 code unit offsets (what
// the page and the protocol use). A leading byte order mark is not counted, matching a
// TextDecoder. An invalid byte counts as one unit, as the decoder's replacement character does.
// ASCII text keeps no table.
class Utf16Map {
public:
    explicit Utf16Map(std::string_view utf8);

    [[nodiscard]] std::uint32_t to_utf16(std::uint32_t byte) const;
    // The byte offset of a UTF-16 offset. An offset inside a surrogate pair moves to the next
    // code point boundary; one past the end clamps to the end.
    [[nodiscard]] std::uint32_t to_byte(std::uint32_t unit) const;
    [[nodiscard]] std::uint32_t bom_bytes() const {
        return bom_;
    }

private:
    std::uint32_t size_ = 0;
    std::uint32_t bom_ = 0;
    // Units before each byte (relative to the BOM end); empty for ASCII text.
    std::vector<std::uint32_t> units_;
};

} // namespace tessera::compiler::analysis
