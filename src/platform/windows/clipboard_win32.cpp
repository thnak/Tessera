// SPDX-License-Identifier: MIT

#include "tessera/hal/clipboard.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <windows.h>

namespace tessera::hal {
namespace {

// OpenClipboard can fail while another process owns the clipboard. The retry budget is bounded and
// short: at most 4 attempts with 1 ms between them (<= 3 ms of waiting), so a busy clipboard fails
// as a value instead of spinning on the UI thread. ADR-021 forbids indefinite blocking.
constexpr int kOpenAttempts = 4;
constexpr DWORD kOpenSleepMs = 1;

bool open_clipboard() {
    for (int attempt = 0; attempt < kOpenAttempts; ++attempt) {
        if (::OpenClipboard(nullptr) != 0) {
            return true;
        }
        if (attempt + 1 < kOpenAttempts) {
            ::Sleep(kOpenSleepMs);
        }
    }
    return false;
}

std::expected<std::string, ClipboardError> wide_to_utf8(const wchar_t* text, std::size_t chars) {
    if (chars == 0) {
        return std::string{};
    }
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(chars), nullptr, 0,
                                             nullptr, nullptr);
    if (needed <= 0) {
        return std::unexpected(ClipboardError::Failed);
    }
    if (static_cast<std::size_t>(needed) > kMaxClipboardBytes) {
        return std::unexpected(ClipboardError::TooLarge);
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(chars), out.data(), needed,
                              nullptr, nullptr) <= 0) {
        return std::unexpected(ClipboardError::Failed);
    }
    return out;
}

std::expected<std::wstring, ClipboardError> utf8_to_wide(std::string_view text) {
    if (text.empty()) {
        return std::wstring{};
    }
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return std::unexpected(ClipboardError::Failed);
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), out.data(), needed) <= 0) {
        return std::unexpected(ClipboardError::Failed);
    }
    return out;
}

class Win32Clipboard final : public Clipboard {
public:
    std::expected<std::string, ClipboardError> read_text() override {
        if (!open_clipboard()) {
            return std::unexpected(ClipboardError::Busy);
        }
        struct Guard {
            ~Guard() {
                ::CloseClipboard();
            }
        } guard;

        HANDLE handle = ::GetClipboardData(CF_UNICODETEXT);
        if (handle == nullptr) {
            return std::string{}; // nothing on the clipboard: a clean empty read
        }
        const SIZE_T bytes = ::GlobalSize(handle);
        if (bytes == 0) {
            return std::string{};
        }
        auto* text = static_cast<const wchar_t*>(::GlobalLock(handle));
        if (text == nullptr) {
            return std::unexpected(ClipboardError::Failed);
        }
        const std::size_t capacity = bytes / sizeof(wchar_t);
        std::size_t chars = 0;
        while (chars < capacity && text[chars] != L'\0') {
            ++chars;
        }
        const auto result = wide_to_utf8(text, chars);
        ::GlobalUnlock(handle);
        return result;
    }

    std::expected<void, ClipboardError> write_text(std::string_view text) override {
        if (text.size() > kMaxClipboardBytes) {
            return std::unexpected(ClipboardError::TooLarge);
        }
        const auto wide = utf8_to_wide(text);
        if (!wide.has_value()) {
            return std::unexpected(wide.error());
        }
        if (!open_clipboard()) {
            return std::unexpected(ClipboardError::Busy);
        }
        struct Guard {
            ~Guard() {
                ::CloseClipboard();
            }
        } guard;

        if (::EmptyClipboard() == 0) {
            return std::unexpected(ClipboardError::Failed);
        }
        const std::size_t chars = wide->size() + 1; // include the NUL terminator
        HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, chars * sizeof(wchar_t));
        if (memory == nullptr) {
            return std::unexpected(ClipboardError::Failed);
        }
        void* destination = ::GlobalLock(memory);
        if (destination == nullptr) {
            ::GlobalFree(memory);
            return std::unexpected(ClipboardError::Failed);
        }
        std::memcpy(destination, wide->c_str(), chars * sizeof(wchar_t));
        ::GlobalUnlock(memory);
        if (::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            ::GlobalFree(memory);
            return std::unexpected(ClipboardError::Failed);
        }
        // Ownership transferred to the system on success; do not free `memory`.
        return {};
    }
};

} // namespace

std::unique_ptr<Clipboard> create_native_clipboard() {
    return std::make_unique<Win32Clipboard>();
}

} // namespace tessera::hal

#endif // _WIN32
