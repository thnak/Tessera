// SPDX-License-Identifier: MIT
//
// Windows UI Automation pattern providers: Invoke, Value, and SelectionItem. Pattern methods only
// validate and queue a UI-thread action through the provider ring; no action runs on the bridge
// thread.

#include "uia_com_provider.hpp"

#if !defined(_WIN32)
#error "uia_com_patterns.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <string>
#include <windows.h>

namespace tessera::ui::a11y::win {

HRESULT STDMETHODCALLTYPE UiaComProvider::Invoke() {
    return run_service([this](UiaProvider& provider) -> HRESULT {
        const auto requested = provider.request_action(index_, Action::Press);
        return requested ? S_OK : hresult_for(requested.error());
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::SetValue(LPCWSTR value) {
    if (value == nullptr) {
        return E_INVALIDARG;
    }
    char buffer[kMaxTextBytes]{};
    const int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer,
                                            static_cast<int>(sizeof(buffer)), nullptr, nullptr);
    if (written <= 0) {
        return E_FAIL;
    }
    const std::string_view text(buffer, static_cast<std::size_t>(written - 1));
    return run_service([this, text](UiaProvider& provider) -> HRESULT {
        const auto requested = provider.request_action(index_, Action::SetValue, 0.0, text);
        return requested ? S_OK : hresult_for(requested.error());
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_Value(BSTR* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    std::string value;
    const HRESULT result = run_service([this, &value](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        value.assign(value_of(*element));
        return S_OK;
    });
    if (FAILED(result)) {
        return result;
    }
    return set_bstr_owned(out, value);
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_IsReadOnly(BOOL* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    return run_service([this, out](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        *out = has_pattern(element->patterns, UiaPatterns::Value) ? FALSE : TRUE;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::Select() {
    // Selecting an item is its activation in the V1 semantics model; multi-select is not modelled.
    return run_service([this](UiaProvider& provider) -> HRESULT {
        const auto requested = provider.request_action(index_, Action::Press);
        return requested ? S_OK : hresult_for(requested.error());
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::AddToSelection() {
    // The semantics model has no multi-select yet; report it rather than faking success.
    return static_cast<HRESULT>(UIA_E_NOTSUPPORTED);
}

HRESULT STDMETHODCALLTYPE UiaComProvider::RemoveFromSelection() {
    return static_cast<HRESULT>(UIA_E_NOTSUPPORTED);
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_IsSelected(BOOL* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    return run_service([this, out](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        *out = element->selected ? TRUE : FALSE;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_SelectionContainer(IRawElementProviderSimple** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (bridge_ == nullptr) {
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    }
    A11yIndex parent = kInvalidIndex;
    const auto posted = bridge_->run_on_bridge(
        [this, &parent](UiaProvider& provider) { parent = provider.parent_of(index_); },
        kServiceWaitMs);
    if (!posted) {
        return hresult_for(posted.error());
    }
    if (parent == kInvalidIndex) {
        return S_OK;
    }
    UiaComProvider* container = make_provider(parent);
    if (container == nullptr) {
        return E_OUTOFMEMORY;
    }
    *out = static_cast<::IRawElementProviderSimple*>(container);
    return S_OK;
}

} // namespace tessera::ui::a11y::win
