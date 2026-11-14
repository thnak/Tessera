// SPDX-License-Identifier: MIT
//
// Pointer shapes through the fake window (issue #191, docs/specs/window-chrome-v1.md section 8.5):
// the `hal::Window` seam as the app driver and the headless harness see it. A real platform
// installs the native cursor on its next event-loop pass; the fake resolves it synchronously and
// reports the same normalized value, so the request/refusal contract and the metrics a caller reads
// are covered without a display.
//
// The app-level half (the one-shot TESSERA_WINDOW_CURSOR_UNSUPPORTED diagnostic and the change-only
// request) is `app::detail::apply_cursor`, which is a plain function over any `hal::Window`, so the
// fake drives it here instead of needing a Vulkan-backed WindowHost.

#include "fake_hal.hpp"
#include "tessera/hal/cutout.hpp"
#include "tessera/ui/style/cursor_value.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

// The app's cursor plumbing is private to src/app; this test links its header directly, like
// `window_config_tests` links the private app headers.
#include "window_cursor.hpp"

namespace {

using namespace tessera;
using harness::testing::FakeWindow;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_cursor_fake: " << message << "\n";
        ++failures;
    }
    return condition;
}

} // namespace

int main() {
    // A fresh window supports pointer shapes and reports the platform arrow.
    {
        FakeWindow window;
        expect(window.supports_cursor(), "the fake supports pointer shapes by default");
        expect(window.metrics().cursor == hal::CursorShape::Default,
               "a fresh window reports the default arrow");
        expect(window.cursor_requests() == 0, "a fresh window has made no cursor request");
    }

    // Round trip: every shape the app can ask for is visible in the metrics, and asking the same
    // shape again still reaches the platform (the fake counts it; the app is what dedupes).
    {
        FakeWindow window;
        window.set_cursor(hal::CursorShape::Pointer);
        expect(window.metrics().cursor == hal::CursorShape::Pointer,
               "a pointer request is reported as the resolved shape");
        expect(window.cursor_requests() == 1, "the request reached the platform");
        window.set_cursor(hal::CursorShape::Text);
        expect(window.metrics().cursor == hal::CursorShape::Text, "a later shape overrides");
        window.set_cursor(hal::CursorShape::Hidden);
        expect(window.metrics().cursor == hal::CursorShape::Hidden,
               "CSS cursor: none resolves to the hidden shape");
        window.set_cursor(hal::CursorShape::Default);
        expect(window.metrics().cursor == hal::CursorShape::Default,
               "returning to the default arrow is reported");
        expect(window.cursor_requests() == 4, "every request is counted");
    }

    // A platform with no pointer-shape source refuses the request: the resolved shape stays as it
    // was, which is what the app's one-shot diagnostic decides from.
    {
        FakeWindow window;
        window.set_cursor_supported(false);
        expect(!window.supports_cursor(), "the scripted capability is reported");
        window.set_cursor(hal::CursorShape::Pointer);
        expect(window.metrics().cursor == hal::CursorShape::Default,
               "a refused request leaves the resolved shape alone");
    }

    // The app-level request path: only a changed shape reaches the platform, and an unsupported
    // platform reports exactly one diagnostic (the return value), never one per frame.
    {
        FakeWindow window;
        hal::CursorShape last = hal::CursorShape::Default;
        bool reported = false;

        // The app maps the scene's kind first, so this exercises the whole chain.
        const hal::CursorShape wanted = app::detail::to_hal_cursor(ui::style::CursorKind::Pointer);
        expect(wanted == hal::CursorShape::Pointer, "the app maps CursorKind::Pointer");
        expect(!app::detail::apply_cursor(window, wanted, last, reported),
               "a supported request reports nothing");
        expect(window.metrics().cursor == hal::CursorShape::Pointer,
               "the supported request reaches the platform");
        expect(window.cursor_requests() == 1, "the supported request is one platform call");

        // A second apply of the same wanted shape makes no platform call at all.
        expect(!app::detail::apply_cursor(window, wanted, last, reported),
               "an unchanged shape reports nothing");
        expect(window.cursor_requests() == 1,
               "an unchanged hover makes no platform call (no per-frame syscall)");

        // Back to the default: one more call.
        expect(!app::detail::apply_cursor(window, hal::CursorShape::Default, last, reported),
               "a return to the arrow reports nothing");
        expect(window.cursor_requests() == 2, "the changed shape is one more platform call");
    }

    // The one-shot diagnostic: an unsupported platform prints once, and later changes stay silent.
    {
        FakeWindow window;
        window.set_cursor_supported(false);
        hal::CursorShape last = hal::CursorShape::Default;
        bool reported = false;
        expect(app::detail::apply_cursor(window, hal::CursorShape::Pointer, last, reported),
               "the first unsupported request prints the diagnostic");
        expect(reported, "the one-shot flag is set");
        expect(!app::detail::apply_cursor(window, hal::CursorShape::Text, last, reported),
               "a later unsupported request prints nothing");
        expect(!app::detail::apply_cursor(window, hal::CursorShape::Default, last, reported),
               "returning to the arrow prints nothing either");
        expect(window.cursor_requests() == 0,
               "an unsupported platform is never asked (supports_cursor gates the call)");
    }

    // Every CursorKind maps onto a shape, `Auto` and `Default` reach the same arrow, and `None` is
    // the hidden shape. A kind that mapped to nothing would be a compiler warning in the switch,
    // not a runtime fallback here.
    {
        using ui::style::CursorKind;
        constexpr CursorKind kKindArray[] = {
            CursorKind::Auto,        CursorKind::Default,    CursorKind::None,
            CursorKind::ContextMenu, CursorKind::Help,       CursorKind::Pointer,
            CursorKind::Progress,    CursorKind::Wait,       CursorKind::Cell,
            CursorKind::Crosshair,   CursorKind::Text,       CursorKind::VerticalText,
            CursorKind::Alias,       CursorKind::Copy,       CursorKind::Move,
            CursorKind::NoDrop,      CursorKind::NotAllowed, CursorKind::Grab,
            CursorKind::Grabbing,    CursorKind::AllScroll,  CursorKind::ZoomIn,
            CursorKind::ZoomOut,     CursorKind::ResizeN,    CursorKind::ResizeE,
            CursorKind::ResizeS,     CursorKind::ResizeW,    CursorKind::ResizeNe,
            CursorKind::ResizeNw,    CursorKind::ResizeSe,   CursorKind::ResizeSw,
            CursorKind::ResizeNs,    CursorKind::ResizeEw,   CursorKind::ResizeNesw,
            CursorKind::ResizeNwse,  CursorKind::ColResize,  CursorKind::RowResize,
        };
        bool all_mapped = true;
        for (const CursorKind kind : kKindArray) {
            const hal::CursorShape shape = app::detail::to_hal_cursor(kind);
            // Every kind maps into the HAL enum's valid range: a shape cast past the end would be
            // an out-of-range request a platform could not answer.
            if (static_cast<std::size_t>(shape) >= hal::kCursorShapeCount) {
                all_mapped = false;
            }
        }
        expect(all_mapped, "every CursorKind maps onto an in-range CursorShape");
        expect(app::detail::to_hal_cursor(CursorKind::Auto) == hal::CursorShape::Default,
               "auto resolves to the platform arrow");
        expect(app::detail::to_hal_cursor(CursorKind::Default) == hal::CursorShape::Default,
               "default resolves to the platform arrow");
        expect(app::detail::to_hal_cursor(CursorKind::None) == hal::CursorShape::Hidden,
               "CSS none resolves to the hidden shape");
    }

    // The hovered-node helper: nothing hovered is the arrow, a hovered node carries its own kind.
    {
        expect(app::detail::hovered_cursor(nullptr) == hal::CursorShape::Default,
               "no hovered node asks for the arrow");
        ui::Node node(1);
        node.set_cursor(ui::style::CursorKind::Text);
        expect(app::detail::hovered_cursor(&node) == hal::CursorShape::Text,
               "a hovered node's resolved kind is mapped");
    }

    // The deterministic metrics dump carries the shape, so a harness script can assert it without a
    // display.
    {
        FakeWindow window;
        expect(hal::format_window_metrics(window.metrics()).find("cursor default\n") !=
                   std::string::npos,
               "the default dump carries the arrow");
        window.set_cursor(hal::CursorShape::ColResize);
        expect(hal::format_window_metrics(window.metrics()).find("cursor col-resize\n") !=
                   std::string::npos,
               "the dump carries the resolved shape");
    }

    if (failures == 0) {
        std::cout << "window_cursor_fake: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
