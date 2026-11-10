// SPDX-License-Identifier: MIT
//
// Windows UI Automation provider: COM identity, property values, and pattern advertisement. The
// object is created on the bridge thread and every call marshals back there; fragment navigation
// and pattern calls live in `uia_com_fragment.cpp` and `uia_com_patterns.cpp`.

#include "tessera/ui/accessibility/uia_mapping.hpp"
#include "uia_com_provider.hpp"

#if !defined(_WIN32)
#error "uia_com.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <new>
#include <string>
#include <windows.h>

namespace tessera::ui::a11y::win {

HRESULT hresult_for(UiaError error) noexcept {
    switch (error) {
    case UiaError::None:
        return S_OK;
    case UiaError::OutOfMemory:
        return E_OUTOFMEMORY;
    case UiaError::ElementNotEnabled:
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED);
    case UiaError::PatternNotSupported:
        return static_cast<HRESULT>(UIA_E_NOTSUPPORTED);
    case UiaError::UnknownNode:
    case UiaError::NoSnapshot:
    case UiaError::NotStarted:
    case UiaError::Timeout:
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    case UiaError::ApartmentUnavailable:
    case UiaError::ComFailure:
    case UiaError::ThreadFailed:
    case UiaError::AlreadyStarted:
    case UiaError::ThreadViolation:
    case UiaError::QueueFull:
    case UiaError::Internal:
    case UiaError::UnsupportedPlatform:
        return E_FAIL;
    }
    return E_FAIL;
}

HRESULT set_bool(VARIANT* out, bool value) {
    out->vt = VT_BOOL;
    out->boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

HRESULT set_bstr_owned(BSTR* out, std::string_view text) {
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    BSTR value = SysAllocStringLen(nullptr, static_cast<UINT>(length));
    if (value == nullptr) {
        return E_OUTOFMEMORY;
    }
    if (length > 0) {
        (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value,
                                  length);
    }
    *out = value;
    return S_OK;
}

HRESULT set_bstr(VARIANT* out, std::string_view text) {
    BSTR value = nullptr;
    const HRESULT result = set_bstr_owned(&value, text);
    if (FAILED(result)) {
        return result;
    }
    out->vt = VT_BSTR;
    out->bstrVal = value;
    return S_OK;
}

HRESULT set_rectangle(VARIANT* out, const rendering::rhi::Rect& bounds) {
    SAFEARRAY* array = SafeArrayCreateVector(VT_R8, 0, 4);
    if (array == nullptr) {
        return E_OUTOFMEMORY;
    }
    double* values = nullptr;
    if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values)))) {
        SafeArrayDestroy(array);
        return E_FAIL;
    }
    values[0] = bounds.x;
    values[1] = bounds.y;
    values[2] = bounds.width;
    values[3] = bounds.height;
    SafeArrayUnaccessData(array);
    out->vt = VT_ARRAY | VT_R8;
    out->parray = array;
    return S_OK;
}

HRESULT UiaComProvider::run_service(const std::function<HRESULT(UiaProvider&)>& fn) noexcept {
    if (bridge_ == nullptr) {
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    }
    HRESULT result = S_OK;
    try {
        const auto posted = bridge_->run_on_bridge(
            [&fn, &result](UiaProvider& provider) { result = fn(provider); }, kServiceWaitMs);
        if (!posted) {
            return hresult_for(posted.error());
        }
    } catch (...) {
        // Boundary: never let an exception leave the COM/FFI seam (ADR-025).
        return E_FAIL;
    }
    return result;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (InlineIsEqualGUID(riid, IID_IUnknown) ||
        InlineIsEqualGUID(riid, IID_IRawElementProviderSimple)) {
        *out = static_cast<::IRawElementProviderSimple*>(this);
    } else if (InlineIsEqualGUID(riid, IID_IRawElementProviderFragment)) {
        *out = static_cast<::IRawElementProviderFragment*>(this);
    } else if (InlineIsEqualGUID(riid, IID_IRawElementProviderFragmentRoot)) {
        if (index_ != 0) {
            return E_NOINTERFACE;
        }
        *out = static_cast<::IRawElementProviderFragmentRoot*>(this);
    } else if (InlineIsEqualGUID(riid, IID_IInvokeProvider)) {
        *out = static_cast<::IInvokeProvider*>(this);
    } else if (InlineIsEqualGUID(riid, IID_IValueProvider)) {
        *out = static_cast<::IValueProvider*>(this);
    } else if (InlineIsEqualGUID(riid, IID_ISelectionItemProvider)) {
        *out = static_cast<::ISelectionItemProvider*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_ProviderOptions(ProviderOptions* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    // A free-threaded, server-side provider: UIA may call from any client thread, and every call is
    // marshaled onto the one bridge thread inside the methods.
    *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider |
                                        ProviderOptions_UseComThreading);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::GetPatternProvider(PATTERNID pattern, IUnknown** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    return run_service([this, pattern, out](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        IUnknown* match = nullptr;
        switch (pattern) {
        case kUiaInvokePatternId:
            if (has_pattern(element->patterns, UiaPatterns::Invoke)) {
                match = static_cast<::IInvokeProvider*>(this);
            }
            break;
        case kUiaValuePatternId:
            if (has_pattern(element->patterns, UiaPatterns::Value)) {
                match = static_cast<::IValueProvider*>(this);
            }
            break;
        case kUiaSelectionItemPatternId:
            if (has_pattern(element->patterns, UiaPatterns::SelectionItem)) {
                match = static_cast<::ISelectionItemProvider*>(this);
            }
            break;
        default:
            break;
        }
        if (match != nullptr) {
            match->AddRef();
        }
        *out = match;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::GetPropertyValue(PROPERTYID property, VARIANT* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    VariantInit(out);
    // Properties that do not depend on a live snapshot stay answerable while the bridge is down.
    if (property == kUiaIsOffscreenPropertyId) {
        return set_bool(out, false);
    }
    if (property == kUiaFrameworkIdPropertyId) {
        return set_bstr(out, kUiaFrameworkId);
    }
    return run_service([this, property, out](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        switch (property) {
        case kUiaNamePropertyId:
            return set_bstr(out, name_of(*element));
        case kUiaControlTypePropertyId:
            out->vt = VT_I4;
            out->lVal = static_cast<LONG>(element->control_type);
            return S_OK;
        case kUiaIsEnabledPropertyId:
            return set_bool(out, element->enabled);
        case kUiaBoundingRectanglePropertyId:
            return set_rectangle(out, element->bounds);
        case kUiaHasKeyboardFocusPropertyId:
            return set_bool(out, element->focused);
        case kUiaAutomationIdPropertyId: {
            const std::string id = std::to_string(element->automation_id);
            return set_bstr(out, id);
        }
        case kUiaHelpTextPropertyId:
            return set_bstr(out, description_of(*element));
        case kUiaValueValuePropertyId:
            return set_bstr(out, value_of(*element));
        case kUiaValueIsReadOnlyPropertyId:
            return set_bool(out, !has_pattern(element->patterns, UiaPatterns::Value));
        case kUiaSelectionItemIsSelectedPropertyId:
            return set_bool(out, element->selected);
        default:
            return S_OK; // VT_EMPTY: the provider does not expose this property.
        }
    });
}

HRESULT STDMETHODCALLTYPE
UiaComProvider::get_HostRawElementProvider(IRawElementProviderSimple** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    // The HAL host (WM_GETOBJECT/UiaReturnRawElementProvider) supplies the host provider later; a
    // fragment provider that is not hosted returns null.
    *out = nullptr;
    return S_OK;
}

::IRawElementProviderSimple* create_uia_provider(UiaBridge& bridge, A11yIndex index) {
    UiaComProvider* provider = new (std::nothrow) UiaComProvider(&bridge, index);
    return static_cast<::IRawElementProviderSimple*>(provider);
}

} // namespace tessera::ui::a11y::win
