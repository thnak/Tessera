// SPDX-License-Identifier: MIT
//
// Windows implementation of the host's UIA platform seam: it owns the COM root provider the window
// returns from WM_GETOBJECT, the `UiaHostProviderFromHwnd` host element that connects the root to
// the window, a bounded per-node provider cache used for events, and the event raisers. Every
// method runs on the bridge thread, except `set_host_handle`/`host_element_status`, which only
// touch atomics.

#include "tessera/ui/accessibility/uia_host.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "uia_com.hpp"

#if !defined(_WIN32)
#error "uia_platform_win.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <array>
#include <atomic>
#include <new>
#include <windows.h>

namespace tessera::ui::a11y {
namespace {

class WindowsUiaPlatform final : public UiaPlatform, private win::HostElementSource {
public:
    void* create_root(UiaBridge& bridge) override {
        bridge_ = &bridge;
        // Snapshot index 0 is the pre-order root, so it is the window's root element whenever a
        // tree exists. Before the first publish the provider answers `UIA_E_ELEMENTNOTAVAILABLE`,
        // which a client retries after the StructureChanged event that follows publication.
        return provider_for(0);
    }

    void destroy_root() override {
        release_host_element();
        for (auto*& provider : providers_) {
            if (provider != nullptr) {
                provider->Release();
                provider = nullptr;
            }
        }
        bridge_ = nullptr;
        // A restart re-arms the wiring: the window (and its handle) may still exist.
        host_state_.store(host_handle_.load(std::memory_order_acquire) != nullptr
                              ? UiaHostElementState::Pending
                              : UiaHostElementState::Unavailable,
                          std::memory_order_release);
    }

    void set_host_handle(hal::AccessibilityHostHandle handle) override {
        // Window thread: store only. `UiaHostProviderFromHwnd` is called on the bridge thread by
        // `acquire`, so this never calls COM, never blocks, and cannot race the provider object.
        void* token = handle.token;
        void* previous = host_handle_.exchange(token, std::memory_order_acq_rel);
        if (previous == token) {
            return; // idempotent: same handle, same wiring, no re-arm
        }
        host_failures_.store(0, std::memory_order_relaxed);
        host_state_.store(token != nullptr ? UiaHostElementState::Pending
                                           : UiaHostElementState::Unavailable,
                          std::memory_order_release);
        // The old cached provider (if any) is released on the bridge thread by the next `acquire`,
        // including when the handle was cleared.
        host_rebuild_.store(true, std::memory_order_release);
    }

    [[nodiscard]] UiaHostElementStatus host_element_status() const override {
        UiaHostElementStatus status;
        status.handle_installed = host_handle_.load(std::memory_order_acquire) != nullptr;
        status.state = host_state_.load(std::memory_order_acquire);
        status.failures = host_failures_.load(std::memory_order_relaxed);
        if (!status.handle_installed) {
            status.state = UiaHostElementState::Unavailable;
        } else if (status.state == UiaHostElementState::Unavailable) {
            status.state = UiaHostElementState::Pending; // install/store race, not a failure
        }
        return status;
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
    // Bridge thread: the host element source for the root provider's get_HostRawElementProvider.
    win::HostElement acquire() override {
        if (host_rebuild_.exchange(false, std::memory_order_acq_rel)) {
            release_host_element();
        }
        // A failed build is latched per handle: a client that retries gets the same E_FAIL instead
        // of another UiaHostProviderFromHwnd call on every query.
        if (host_state_.load(std::memory_order_acquire) == UiaHostElementState::Failed) {
            return {nullptr, win::HostElementState::Failed};
        }
        if (host_provider_ != nullptr) {
            host_provider_->AddRef();
            return {host_provider_, win::HostElementState::Ready};
        }
        void* token = host_handle_.load(std::memory_order_acquire);
        if (token == nullptr) {
            return {};
        }
        ::IRawElementProviderSimple* provider = nullptr;
        const HRESULT result = ::UiaHostProviderFromHwnd(static_cast<HWND>(token), &provider);
        if (FAILED(result) || provider == nullptr) {
            host_failures_.fetch_add(1, std::memory_order_relaxed);
            host_state_.store(UiaHostElementState::Failed, std::memory_order_release);
            return {nullptr, win::HostElementState::Failed};
        }
        // `provider` is the create reference: the cache owns it and each caller gets an AddRef.
        host_provider_ = provider;
        host_provider_->AddRef();
        host_state_.store(UiaHostElementState::Ready, std::memory_order_release);
        return {host_provider_, win::HostElementState::Ready};
    }

    // Bridge thread only.
    void release_host_element() {
        if (host_provider_ != nullptr) {
            host_provider_->Release();
            host_provider_ = nullptr;
        }
    }

    // One provider object per snapshot index, created lazily on the bridge thread and released
    // with the root. An out-of-range index (a stale event) resolves to the root provider.
    ::IRawElementProviderSimple* provider_for(A11yIndex index) {
        if (bridge_ == nullptr) {
            return nullptr;
        }
        const A11yIndex slot = index < providers_.size() ? index : static_cast<A11yIndex>(0);
        if (providers_[slot] == nullptr) {
            providers_[slot] = win::create_uia_provider(*bridge_, slot, this);
        }
        return providers_[slot];
    }

    UiaBridge* bridge_ = nullptr;
    // Consulted on the bridge thread; installed by the window thread through atomics only.
    std::atomic<void*> host_handle_{nullptr};
    std::atomic<UiaHostElementState> host_state_{UiaHostElementState::Unavailable};
    std::atomic<std::uint64_t> host_failures_{0};
    std::atomic<bool> host_rebuild_{false};
    // Bridge-thread owned.
    ::IRawElementProviderSimple* host_provider_ = nullptr;
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
