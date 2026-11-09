// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace tessera::hal {

// Clipboard seam for editable UI (docs/specs/text-editing-v1.md section 12, docs/design/hal.md).
// UI code only sees this interface: platform code implements it, so no window-system calls live in
// the UI layer (ADR-020 / native-first boundaries).
//
// The seam is deliberately small: UTF-8 text only (rich text/HTML, images and change notifications
// are deferred). Both directions are bounded by `kMaxClipboardBytes`; a larger write fails with
// `TooLarge` and a larger read fails with `TooLarge` instead of silently truncating. Failures are
// values (std::expected), never exceptions or blocking waits.
enum class ClipboardError : std::uint8_t {
    None = 0,
    Unsupported, // no backend for this platform/session (e.g. headless, X11 selection not wired)
    Busy,        // another process holds the clipboard beyond the bounded retry budget
    TooLarge,    // payload exceeds kMaxClipboardBytes
    Failed,      // the platform call failed (lock/allocation/encoding failure)
};

// Maximum UTF-8 bytes read or written through this seam (64 KiB). Chosen so a paste is a single
// bounded command against the edit buffer, which itself caps at kMaxEditTextBytes.
inline constexpr std::size_t kMaxClipboardBytes = 64 * 1024;

class Clipboard {
public:
    virtual ~Clipboard() = default;

    // Reads the current clipboard as UTF-8. An empty clipboard succeeds with an empty string.
    [[nodiscard]] virtual std::expected<std::string, ClipboardError> read_text() = 0;
    // Replaces the clipboard with UTF-8 text (bounded by kMaxClipboardBytes).
    [[nodiscard]] virtual std::expected<void, ClipboardError> write_text(std::string_view text) = 0;
};

// In-memory implementation used by tests and by platforms without a native backend. It is
// deterministic and owns its storage; an empty write stores an empty clipboard.
class MemoryClipboard final : public Clipboard {
public:
    [[nodiscard]] std::expected<std::string, ClipboardError> read_text() override;
    [[nodiscard]] std::expected<void, ClipboardError> write_text(std::string_view text) override;

    [[nodiscard]] const std::string& text() const {
        return text_;
    }

private:
    std::string text_;
};

// Unsupported backend: returns ClipboardError::Unsupported for both operations. Used on platforms
// whose native integration is deferred (Linux/X11 selection protocol, macOS, Android) and on
// headless sessions. UI code treats Unsupported like an unavailable clipboard: cut/paste are
// no-ops.
class UnsupportedClipboard final : public Clipboard {
public:
    [[nodiscard]] std::expected<std::string, ClipboardError> read_text() override;
    [[nodiscard]] std::expected<void, ClipboardError> write_text(std::string_view text) override;
};

std::unique_ptr<Clipboard> create_memory_clipboard();
// Native backend for the platform this library was built for (Win32 today; UnsupportedClipboard
// elsewhere). The caller owns the returned object.
std::unique_ptr<Clipboard> create_native_clipboard();

} // namespace tessera::hal
