// SPDX-License-Identifier: MIT
//
// Windows-only: the Win32 window routes WM_GETOBJECT through the installed accessibility handshake
// and falls back to DefWindowProc when no root is ready. The test installs a probe that always
// declines, so no fake provider pointer ever reaches UiaReturnRawElementProvider; it only proves
// the seam is consulted with the right object id and that the default path still answers.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <windows.h>

namespace {

struct Probe {
    int calls = 0;
    std::intptr_t last_object_id = 0;
    bool decline = true;
};

bool answer_thunk(void* context, std::intptr_t object_id, void*& raw_provider) {
    auto* probe = static_cast<Probe*>(context);
    ++probe->calls;
    probe->last_object_id = object_id;
    raw_provider = nullptr;
    return !probe->decline;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y uia window: " << message << "\n";
    }
    return condition;
}

// Sends WM_GETOBJECT the way the system does for a UIA client. The return value is intentionally
// not asserted: the contract under test is which handler answered, not what DefWindowProc returns.
void query(HWND window, std::intptr_t object_id) {
    (void)SendMessageW(window, WM_GETOBJECT, 0, static_cast<LPARAM>(object_id));
}

} // namespace

int main() {
    using tessera::ui::a11y::kUiaRootObjectId;

    const std::string title = "Tessera UIA host test " + std::to_string(GetCurrentProcessId());
    tessera::hal::WindowDesc desc;
    desc.title = title;
    desc.width = 160;
    desc.height = 120;
    desc.visible = false;

    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 1;
    }
    const HWND hwnd = FindWindowA(nullptr, title.c_str());
    if (!expect(hwnd != nullptr, "native window exists")) {
        return 2;
    }

    // Without a hook the window keeps the platform default (DefWindowProc / MSAA) answer.
    query(hwnd, kUiaRootObjectId);

    Probe probe;
    window->set_accessibility_root({&probe, &answer_thunk});
    query(hwnd, kUiaRootObjectId);
    if (!expect(probe.calls == 1, "WM_GETOBJECT reaches the accessibility handshake") ||
        !expect(probe.last_object_id == kUiaRootObjectId, "the handshake sees the UIA root id")) {
        return 3;
    }

    query(hwnd, -4); // OBJID_CLIENT: the host declines and the default MSAA bridge answers.
    if (!expect(probe.calls == 2, "every WM_GETOBJECT consults the handshake") ||
        !expect(probe.last_object_id == -4, "the handshake sees the OS object id")) {
        return 4;
    }

    // Uninstall: the window must stop consulting the (now dead) context.
    window->set_accessibility_root({});
    query(hwnd, kUiaRootObjectId);
    if (!expect(probe.calls == 2, "an uninstalled hook is not called")) {
        return 5;
    }

    std::cout << "a11y_uia_window_getobject: ok\n";
    return 0;
}
