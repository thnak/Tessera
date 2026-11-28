// SPDX-License-Identifier: MIT
//
// The gallery's page table (issue #528, ADR-047 item 10).
//
// **Why a table and not an `if` chain.** A page used to be three places: a name in the `--page`
// validity check, an `if` branch that built it in `main.cpp`, and a `TESSERA_GALLERY_*_CSS`
// definition in `samples/gallery/CMakeLists.txt`. Every new page edited two shared files, so two
// people could not add two pages at once - which is exactly what the library component phases need
// to do. Adding a page now touches this file (one line) plus the page's own `.tsx`/`.css`.
//
// The same list answers `--page` validity and the build, so the help text, the diagnostic and the
// dispatch cannot disagree about which names exist.
#pragma once

#include "DashPage.hpp"
#include "FormPage.hpp"
#include "Gallery.hpp"
#include "SafeAreaPage.hpp"
#include "SwatchPage.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace gallery {

// Every gallery page uses this root id, so the parity tooling, the goldens and this table agree on
// one number.
constexpr tessera::ui::NodeId kRootId = 900;

// One generated page in a scene root. `Page` is `gallery::Gallery`, `gallery::SafeAreaPage`, and so
// on. It exists so `run_capture` and `run_window` stay templates over "a page in a root" rather
// than knowing every page type.
template<typename Page> struct Scene {
    Scene() {
        page.build(root);
        page.mount();
        page.attach_scheduler(scheduler);
    }

    tessera::ui::Node root{kRootId};
    Page page;
    tessera::ui::UpdateScheduler scheduler;
};

// The page names `--page` accepts, in the order the usage text lists them: `buttons` first because
// it is the default.
//
// **Why this is a separate array from the dispatch.** The usage text and the validity check need
// the names without a `Runner` to instantiate them with, so the names live here and the dispatch
// below is instantiated at the call site. `gallery_page_table_tests` walks both and fails if a name
// here has no branch there, or the reverse - which is the drift a second list would otherwise
// invite.
inline constexpr std::array<std::string_view, 5> kPageNames{"buttons", "safe-area", "form", "dash",
                                                            "swatch"};

// Whether `name` is a page. `--page`'s validation is this and nothing else, so an unknown page and
// a known one are answered by the same list.
[[nodiscard]] constexpr bool is_page(std::string_view name) noexcept {
    for (const std::string_view candidate : kPageNames) {
        if (candidate == name) {
            return true;
        }
    }
    return false;
}

// Whether `name`'s stylesheet is a CSS module (issue #463), which `--emit-module-css` needs: only
// the form page has one, and emitting it from any other page would write that page's classes under
// the form's names. False for a name that is not a page.
[[nodiscard]] constexpr bool page_has_module_css(std::string_view name) noexcept {
    return name == "form";
}

// Runs the named page through `runner`. `Runner` must be callable as `runner(Scene<Page>&)`: it is
// the capture-or-window lambda in `main.cpp`, which captures the parsed arguments by reference, so
// it is a template parameter rather than a `std::function`.
//
// A name that is not a page returns `std::nullopt`; the caller reports the diagnostic before it
// gets here, so this only has to not guess.
template<typename Runner>
[[nodiscard]] std::optional<int> run_page(std::string_view name, const Runner& runner) {
    if (name == "buttons") {
        Scene<Gallery> scene;
        return runner(scene);
    }
    if (name == "safe-area") {
        Scene<SafeAreaPage> scene;
        return runner(scene);
    }
    if (name == "form") {
        Scene<FormPage> scene;
        return runner(scene);
    }
    if (name == "dash") {
        Scene<DashPage> scene;
        return runner(scene);
    }
    if (name == "swatch") {
        Scene<SwatchPage> scene;
        return runner(scene);
    }
    return std::nullopt;
}

} // namespace gallery
