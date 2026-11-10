// SPDX-License-Identifier: MIT
//
// Windows UI Automation fragment navigation: parent/child/sibling links, runtime id, bounds, focus,
// and the fragment-root entry points a client uses to hit-test and read the focused element. Every
// query marshals onto the bridge thread through `UiaBridge::run_on_bridge`.

#include "uia_com_provider.hpp"

#if !defined(_WIN32)
#error "uia_com_fragment.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <expected>
#include <windows.h>

namespace tessera::ui::a11y::win {

HRESULT STDMETHODCALLTYPE UiaComProvider::Navigate(NavigateDirection direction,
                                                   IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (bridge_ == nullptr) {
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    }
    const A11yIndex self = index_;
    A11yIndex target = kInvalidIndex;
    UiaError failure = UiaError::None;
    const auto posted = bridge_->run_on_bridge(
        [self, direction, &target, &failure](UiaProvider& provider) {
            const A11yIndex parent = provider.parent_of(self);
            switch (direction) {
            case NavigateDirection_Parent:
                target = parent;
                break;
            case NavigateDirection_FirstChild: {
                const auto count = provider.child_count(self);
                if (!count) {
                    failure = count.error();
                    break;
                }
                if (*count > 0) {
                    const auto child = provider.child_at(self, 0);
                    if (child) {
                        target = *child;
                    } else {
                        failure = child.error();
                    }
                }
                break;
            }
            case NavigateDirection_LastChild: {
                const auto count = provider.child_count(self);
                if (!count) {
                    failure = count.error();
                    break;
                }
                if (*count > 0) {
                    const auto child =
                        provider.child_at(self, static_cast<std::uint16_t>(*count - 1));
                    if (child) {
                        target = *child;
                    } else {
                        failure = child.error();
                    }
                }
                break;
            }
            case NavigateDirection_NextSibling:
            case NavigateDirection_PreviousSibling: {
                if (parent == kInvalidIndex) {
                    break;
                }
                const auto count = provider.child_count(parent);
                if (!count) {
                    failure = count.error();
                    break;
                }
                const std::uint16_t position = provider.index_in_parent(self);
                if (direction == NavigateDirection_NextSibling) {
                    if (static_cast<std::uint32_t>(position) + 1U < *count) {
                        const auto child =
                            provider.child_at(parent, static_cast<std::uint16_t>(position + 1U));
                        if (child) {
                            target = *child;
                        } else {
                            failure = child.error();
                        }
                    }
                } else if (position > 0) {
                    const auto child =
                        provider.child_at(parent, static_cast<std::uint16_t>(position - 1));
                    if (child) {
                        target = *child;
                    } else {
                        failure = child.error();
                    }
                }
                break;
            }
            default:
                break;
            }
        },
        kServiceWaitMs);
    if (!posted) {
        return hresult_for(posted.error());
    }
    if (failure != UiaError::None) {
        return hresult_for(failure);
    }
    if (target == kInvalidIndex) {
        return S_OK; // null is a valid "no such neighbor" answer.
    }
    UiaComProvider* provider = make_provider(target);
    if (provider == nullptr) {
        return E_OUTOFMEMORY;
    }
    *out = static_cast<::IRawElementProviderFragment*>(provider);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::GetRuntimeId(SAFEARRAY** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    SAFEARRAY* array = SafeArrayCreateVector(VT_I4, 0, 2);
    if (array == nullptr) {
        return E_OUTOFMEMORY;
    }
    int* values = nullptr;
    if (FAILED(SafeArrayAccessData(array, reinterpret_cast<void**>(&values)))) {
        SafeArrayDestroy(array);
        return E_FAIL;
    }
    values[0] = UiaAppendRuntimeId;
    values[1] = static_cast<int>(index_) + 1;
    SafeArrayUnaccessData(array);
    *out = array;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_BoundingRectangle(UiaRect* out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    return run_service([this, out](UiaProvider& provider) -> HRESULT {
        const auto element = provider.element(index_);
        if (!element) {
            return hresult_for(element.error());
        }
        out->left = element->bounds.x;
        out->top = element->bounds.y;
        out->width = element->bounds.width;
        out->height = element->bounds.height;
        return S_OK;
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::GetEmbeddedFragmentRoots(SAFEARRAY** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::SetFocus() {
    return run_service([this](UiaProvider& provider) -> HRESULT {
        const auto requested = provider.request_action(index_, Action::Focus);
        return requested ? S_OK : hresult_for(requested.error());
    });
}

HRESULT STDMETHODCALLTYPE UiaComProvider::get_FragmentRoot(IRawElementProviderFragmentRoot** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (index_ == 0) {
        *out = static_cast<::IRawElementProviderFragmentRoot*>(this);
        AddRef();
        return S_OK;
    }
    UiaComProvider* root = make_provider(0);
    if (root == nullptr) {
        return E_OUTOFMEMORY;
    }
    *out = static_cast<::IRawElementProviderFragmentRoot*>(root);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE
UiaComProvider::ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (bridge_ == nullptr) {
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    }
    A11yIndex target = kInvalidIndex;
    UiaError failure = UiaError::None;
    const auto posted = bridge_->run_on_bridge(
        [x, y, &target, &failure](UiaProvider& provider) {
            const auto hit = provider.hit_test(x, y);
            if (hit) {
                target = *hit;
            } else {
                failure = hit.error();
            }
        },
        kServiceWaitMs);
    if (!posted) {
        return hresult_for(posted.error());
    }
    if (failure != UiaError::None) {
        return hresult_for(failure);
    }
    UiaComProvider* provider = make_provider(target);
    if (provider == nullptr) {
        return E_OUTOFMEMORY;
    }
    *out = static_cast<::IRawElementProviderFragment*>(provider);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE UiaComProvider::GetFocus(IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    if (bridge_ == nullptr) {
        return static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE);
    }
    A11yIndex target = kInvalidIndex;
    UiaError failure = UiaError::None;
    const auto posted = bridge_->run_on_bridge(
        [&target, &failure](UiaProvider& provider) {
            const auto focused = provider.focused();
            if (focused) {
                target = *focused;
            } else {
                failure = focused.error();
            }
        },
        kServiceWaitMs);
    if (!posted) {
        return hresult_for(posted.error());
    }
    if (failure != UiaError::None) {
        return hresult_for(failure);
    }
    UiaComProvider* provider = make_provider(target);
    if (provider == nullptr) {
        return E_OUTOFMEMORY;
    }
    *out = static_cast<::IRawElementProviderFragment*>(provider);
    return S_OK;
}

} // namespace tessera::ui::a11y::win
