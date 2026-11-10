// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/uia_error.hpp"

#include <expected>
#include <memory>
#include <string_view>

namespace tessera::ui::a11y {

// The bridge thread's COM apartment and message-loop seam. `UiaBridge` owns one implementation and
// calls it only from its worker thread: `init` once before any provider work, `pump` once per
// bounded iteration, `shutdown` once after the last iteration. Implementations never throw; every
// failure is a `UiaError` value, because this seam is also the FFI boundary (ADR-025).
class UiaApartment {
public:
    virtual ~UiaApartment() = default;

    // Called on the bridge thread: Windows runs `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`.
    [[nodiscard]] virtual std::expected<void, UiaError> init() = 0;
    // Called on the bridge thread after the last iteration (COM uninitialize).
    virtual void shutdown() = 0;
    // One bounded message-loop pump; the default is a no-op for platforms without a window message
    // queue. It must never block.
    virtual void pump() {}
    // Stable name for diagnostics and tests ("win32-mta", "unsupported").
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// Windows: an MTA apartment with a Win32 message pump. Every other platform: an apartment whose
// `init` reports `UnsupportedPlatform`, so the bridge reports accessibility disabled instead of
// failing, and the headers still configure and compile everywhere.
[[nodiscard]] std::unique_ptr<UiaApartment> create_platform_apartment();

} // namespace tessera::ui::a11y
