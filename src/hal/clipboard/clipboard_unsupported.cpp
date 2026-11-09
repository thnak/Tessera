// SPDX-License-Identifier: MIT

#include "tessera/hal/clipboard.hpp"

namespace tessera::hal {

// Non-Windows platforms: the clipboard backend is deferred this slice. Linux/X11 needs the
// selection protocol (owner/window/INCR transfer), macOS uses NSPasteboard, Android uses the
// ClipboardManager bridge; all report Unsupported until their slices land, so cut/paste are clean
// no-ops instead of silent failures.
std::unique_ptr<Clipboard> create_native_clipboard() {
    return std::make_unique<UnsupportedClipboard>();
}

} // namespace tessera::hal
