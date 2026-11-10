// SPDX-License-Identifier: MIT

#include "uia_sdk_probe.hpp"

#if !defined(_WIN32)
#error "uia_sdk_probe.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include "tessera/ui/accessibility/uia_mapping.hpp"

// The UIA provider interfaces and ids are the one place this layer touches the Windows SDK, and
// only in the Windows translation units. The MIDL-generated UIA headers pull in the RPC/OLE base
// types they need, so this block compiles on its own (clang-format keeps `windows.h` last).
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>
#include <windows.h>

namespace tessera::ui::a11y::win {
namespace {

// Property ids the provider answers, checked against the SDK spelling.
static_assert(kUiaBoundingRectanglePropertyId == UIA_BoundingRectanglePropertyId);
static_assert(kUiaControlTypePropertyId == UIA_ControlTypePropertyId);
static_assert(kUiaNamePropertyId == UIA_NamePropertyId);
static_assert(kUiaHasKeyboardFocusPropertyId == UIA_HasKeyboardFocusPropertyId);
static_assert(kUiaIsEnabledPropertyId == UIA_IsEnabledPropertyId);
static_assert(kUiaAutomationIdPropertyId == UIA_AutomationIdPropertyId);
static_assert(kUiaHelpTextPropertyId == UIA_HelpTextPropertyId);
static_assert(kUiaIsOffscreenPropertyId == UIA_IsOffscreenPropertyId);
static_assert(kUiaFrameworkIdPropertyId == UIA_FrameworkIdPropertyId);
static_assert(kUiaValueValuePropertyId == UIA_ValueValuePropertyId);
static_assert(kUiaValueIsReadOnlyPropertyId == UIA_ValueIsReadOnlyPropertyId);
static_assert(kUiaSelectionItemIsSelectedPropertyId == UIA_SelectionItemIsSelectedPropertyId);

// Pattern ids.
static_assert(kUiaInvokePatternId == UIA_InvokePatternId);
static_assert(kUiaSelectionPatternId == UIA_SelectionPatternId);
static_assert(kUiaValuePatternId == UIA_ValuePatternId);
static_assert(kUiaSelectionItemPatternId == UIA_SelectionItemPatternId);

// Control types the role table can return.
static_assert(static_cast<std::int32_t>(UiaControlType::Button) == UIA_ButtonControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::CheckBox) == UIA_CheckBoxControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::ComboBox) == UIA_ComboBoxControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Edit) == UIA_EditControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Hyperlink) == UIA_HyperlinkControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Image) == UIA_ImageControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::ListItem) == UIA_ListItemControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::List) == UIA_ListControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Menu) == UIA_MenuControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::MenuItem) == UIA_MenuItemControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::ProgressBar) ==
              UIA_ProgressBarControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::RadioButton) ==
              UIA_RadioButtonControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Slider) == UIA_SliderControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::TabItem) == UIA_TabItemControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Text) == UIA_TextControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::ToolTip) == UIA_ToolTipControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Group) == UIA_GroupControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Document) == UIA_DocumentControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Window) == UIA_WindowControlTypeId);
static_assert(static_cast<std::int32_t>(UiaControlType::Pane) == UIA_PaneControlTypeId);

} // namespace

bool uia_sdk_probe() noexcept {
    // CoInitializeEx proves `ole32` is linked; UiaClientsAreListening proves `uiautomationcore` is.
    // The probe is read-only: it never registers a provider and never calls a client.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool apartment_ok = SUCCEEDED(init) || init == RPC_E_CHANGED_MODE;
    (void)UiaClientsAreListening();
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return apartment_ok;
}

} // namespace tessera::ui::a11y::win
