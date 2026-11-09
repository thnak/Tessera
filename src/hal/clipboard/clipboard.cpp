// SPDX-License-Identifier: MIT

#include "tessera/hal/clipboard.hpp"

namespace tessera::hal {

std::expected<std::string, ClipboardError> MemoryClipboard::read_text() {
    if (text_.size() > kMaxClipboardBytes) {
        return std::unexpected(ClipboardError::TooLarge);
    }
    return text_;
}

std::expected<void, ClipboardError> MemoryClipboard::write_text(std::string_view text) {
    if (text.size() > kMaxClipboardBytes) {
        return std::unexpected(ClipboardError::TooLarge);
    }
    text_.assign(text.data(), text.size());
    return {};
}

std::expected<std::string, ClipboardError> UnsupportedClipboard::read_text() {
    return std::unexpected(ClipboardError::Unsupported);
}

std::expected<void, ClipboardError> UnsupportedClipboard::write_text(std::string_view) {
    return std::unexpected(ClipboardError::Unsupported);
}

std::unique_ptr<Clipboard> create_memory_clipboard() {
    return std::make_unique<MemoryClipboard>();
}

} // namespace tessera::hal
