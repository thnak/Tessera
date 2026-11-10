// SPDX-License-Identifier: MIT
//
// Windows implementation of the host's UIA platform seam: it owns the COM root provider the window
// returns from WM_GETOBJECT, a bounded per-node provider cache used for events, and the event
// raisers. Every method runs on the bridge thread.

#include "tessera/ui/accessibility/uia_host.hpp"
#include "uia_com.hpp"

#if !defined(_WIN32)
#error "uia_platform_win.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <array>
#include <new>

namespace tessera::ui::a11y {
namespace {

class WindowsUiaPlatform final : public UiaPlatform {
public:
    void* create_root(UiaBridge& bridge) override {
        bridge_ = &bridge;
        // Snapshot index 0 is the pre-order root, so it is the window's root element whenever a
        // tree exists. Before the first publish the provider answers `UIA_E_ELEMENTNOTAVAILABLE`,
        // which a client retries after the StructureChanged event that follows publication.
        return provider_for(0);
    }

    void destroy_root() override {
        for (auto*& provider : providers_) {
            if (provider != nullptr) {
                provider->Release();
                provider = nullptr;
            }
        }
        bridge_ = nullptr;
    }

    UiaError raise(std::span<const UiaEvent> events) override {
        if (bridge_ == nullptr) {
            return UiaError::NotStarted;
        }
        UiaError result = UiaError::None;
        for (const UiaEvent& event : events) {
            ::IRawElementProviderSimple* provider = provider_for(event.node);
            if (provider == nullptr) {
                result = UiaError::OutOfMemory;
                continue;
            }
            switch (event.kind) {
            case UiaEventKind::StructureChanged:
                if (FAILED(UiaRaiseStructureChangedEvent(
                        provider, static_cast<StructureChangeType>(event.property), nullptr, 0))) {
                    result = UiaError::ComFailure;
                }
                break;
            case UiaEventKind::PropertyChanged: {
                // The client re-queries the property; UIA's event only says "something changed".
                VARIANT empty;
                VariantInit(&empty);
                if (FAILED(UiaRaiseAutomationPropertyChangedEvent(
                        provider, static_cast<PROPERTYID>(event.property), empty, empty))) {
                    result = UiaError::ComFailure;
                }
                break;
            }
            case UiaEventKind::FocusChanged:
                if (FAILED(UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId))) {
                    result = UiaError::ComFailure;
                }
                break;
            }
        }
        return result;
    }

private:
    // One provider object per snapshot index, created lazily on the bridge thread and released
    // with the root. An out-of-range index (a stale event) resolves to the root provider.
    ::IRawElementProviderSimple* provider_for(A11yIndex index) {
        if (bridge_ == nullptr) {
            return nullptr;
        }
        const A11yIndex slot = index < providers_.size() ? index : static_cast<A11yIndex>(0);
        if (providers_[slot] == nullptr) {
            providers_[slot] = win::create_uia_provider(*bridge_, slot);
        }
        return providers_[slot];
    }

    UiaBridge* bridge_ = nullptr;
    std::array<::IRawElementProviderSimple*, kMaxNodes> providers_{};
};

} // namespace

std::unique_ptr<UiaPlatform> create_platform_uia() {
    try {
        return std::make_unique<WindowsUiaPlatform>();
    } catch (...) {
        // Boundary: an allocation failure disables accessibility instead of throwing at the host.
        return nullptr;
    }
}

} // namespace tessera::ui::a11y
