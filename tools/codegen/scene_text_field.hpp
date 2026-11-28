// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

#include <cstdint>

namespace tessera::compiler::codegen::cli {

// What the focused text field looks like to the page (issue #601): enough to put the hidden
// editable element on top of it and to report the caret. Offsets are UTF-8 bytes into the field's
// value.
struct PreviewTextField {
    bool focused = false;   // the focused node is an editable text field
    bool composing = false; // an IME preedit is in progress
    bool multiline = false;
    std::uint32_t caret = 0;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    tessera::ui::rhi::Rect box; // the field's box in logical viewport pixels
};

} // namespace tessera::compiler::codegen::cli
