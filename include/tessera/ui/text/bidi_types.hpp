// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

namespace tessera::ui::text {

enum class TextDirection { LeftToRight, RightToLeft };

// One visual run of a line, in visual (left-to-right) order. Byte offsets are absolute in the
// original UTF-8 paragraph text; `level` is the resolved bidi embedding level (odd = RTL).
struct BidiVisualRun {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint8_t level = 0;

    bool operator==(const BidiVisualRun&) const = default;
};

} // namespace tessera::ui::text
