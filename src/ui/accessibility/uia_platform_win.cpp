// SPDX-License-Identifier: MIT
//
// Windows implementation of the host's UIA platform seam: it owns the COM root provider the window
// returns from WM_GETOBJECT. The provider itself lives in `uia_com*.cpp`; this file only decides
// when it is created and released, always on the bridge thread.

#include "tessera/ui/accessibility/uia_host.hpp"
#include "uia_com.hpp"

#if !defined(_WIN32)
#error "uia_platform_win.cpp is Windows-only and must stay behind a WIN32 CMake guard"
#endif

#include <UIAutomationCore.h>
#include <new>

namespace tessera::ui::a11y {
namespace {

class WindowsUiaPlatform final : public UiaPlatform {
public:
    void* create_root(UiaBridge& bridge) override {
        // Snapshot index 0 is the pre-order root, so it is the window's root element whenever a
        // tree exists. Before the first publish the provider answers `UIA_E_ELEMENTNOTAVAILABLE`,
        // which a client retries after the StructureChanged event that follows publication.
        root_ = win::create_uia_provider(bridge, 0);
        return root_;
    }

    void destroy_root() override {
        if (root_ != nullptr) {
            root_->Release();
            root_ = nullptr;
        }
    }

private:
    ::IRawElementProviderSimple* root_ = nullptr;
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
