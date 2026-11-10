// SPDX-License-Identifier: MIT

#pragma once

#include "uia_com.hpp"

#include <atomic>
#include <functional>
#include <new>
#include <string_view>

namespace tessera::ui::a11y::win {

// Liveness bound for one marshaled client call. Provider work is a bounded snapshot read, so a
// client only waits longer when the bridge thread is blocked (a host bug); it then gets a value.
inline constexpr int kServiceWaitMs = 5000;

HRESULT hresult_for(UiaError error) noexcept;
HRESULT set_bool(VARIANT* out, bool value);
HRESULT set_bstr(VARIANT* out, std::string_view text);
HRESULT set_bstr_owned(BSTR* out, std::string_view text);
HRESULT set_rectangle(VARIANT* out, const rendering::rhi::Rect& bounds);

// The COM provider object. One instance per snapshot node; all interface methods marshal onto the
// bridge thread through `run_service`, so no method ever reads the snapshot from a client thread.
class UiaComProvider final : public ::IRawElementProviderSimple,
                             public ::IRawElementProviderFragment,
                             public ::IRawElementProviderFragmentRoot,
                             public ::IInvokeProvider,
                             public ::IValueProvider,
                             public ::ISelectionItemProvider {
public:
    UiaComProvider(UiaBridge* bridge, A11yIndex index)
        : bridge_(bridge),
          index_(index) {}

    // IUnknown.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override {
        return refs_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    // IRawElementProviderSimple.
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override;

    // IRawElementProviderFragment.
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,
                                       IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override;

    // IRawElementProviderFragmentRoot.
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y,
                                                       IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override;

    // IInvokeProvider.
    HRESULT STDMETHODCALLTYPE Invoke() override;

    // IValueProvider.
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override;

    // ISelectionItemProvider.
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* out) override;
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** out) override;

    // Runs `fn` on the bridge thread and maps its result. `fn` returns an HRESULT and may fill
    // caller-owned out-parameters; it runs before this returns, so captures are safe. `noexcept`:
    // this is the COM/FFI boundary (ADR-025).
    HRESULT run_service(const std::function<HRESULT(UiaProvider&)>& fn) noexcept;

    // Creates one provider object for `index` (refcount 1), or null on allocation failure.
    [[nodiscard]] UiaComProvider* make_provider(A11yIndex index) const {
        if (bridge_ == nullptr) {
            return nullptr;
        }
        return new (std::nothrow) UiaComProvider(bridge_, index);
    }

private:
    UiaBridge* bridge_ = nullptr;
    A11yIndex index_ = kInvalidIndex;
    std::atomic<ULONG> refs_{1};
};

} // namespace tessera::ui::a11y::win
