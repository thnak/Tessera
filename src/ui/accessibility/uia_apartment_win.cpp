// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_apartment.hpp"

#if !defined(_WIN32)
#error "uia_apartment_win.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <objbase.h>
#include <windows.h>

namespace tessera::ui::a11y {
namespace {

// The MTA apartment UIA providers must live in: a free-threaded provider can be called from any
// client thread, and `UiaBridge::run_on_bridge` marshals every call back onto this thread. The
// message pump keeps COM's window-message-based paths (and any host-posted messages) moving; the
// bridge loop calls it once per bounded iteration and it never blocks.
class Win32MtaApartment final : public UiaApartment {
public:
    [[nodiscard]] std::expected<void, UiaError> init() override {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            // The thread is already in an STA; UIA providers cannot be served from it.
            return std::unexpected(UiaError::ApartmentUnavailable);
        }
        if (FAILED(result)) {
            return std::unexpected(UiaError::ComFailure);
        }
        return {};
    }

    void shutdown() override {
        CoUninitialize();
    }

    void pump() override {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    [[nodiscard]] std::string_view name() const override {
        return "win32-mta";
    }
};

} // namespace

std::unique_ptr<UiaApartment> create_platform_apartment() {
    return std::make_unique<Win32MtaApartment>();
}

} // namespace tessera::ui::a11y
