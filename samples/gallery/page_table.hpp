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

#include "ControlsPage.hpp"
#include "DashPage.hpp"
#include "FormPage.hpp"
#include "Gallery.hpp"
#include "IconsPage.hpp"
#include "ListsPage.hpp"
#include "SafeAreaPage.hpp"
#include "SwatchPage.hpp"
#include "TablesPage.hpp"
#include "TablesPage.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

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
inline constexpr std::array<std::string_view, 9> kPageNames{
    "buttons", "safe-area", "form", "dash", "swatch", "icons", "lists", "controls", "tables"};

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

// Calls `visit(std::type_identity<Page>{})` for the page named `name` and returns true; false for a
// name that is not a page. This is the one place a name is mapped to a page type, so `run_page` and
// `make_scene` (and anything added later) cannot disagree about which names exist.
template<typename Visitor> [[nodiscard]] bool visit_page(std::string_view name, Visitor&& visit) {
    if (name == "buttons") {
        visit(std::type_identity<Gallery>{});
    } else if (name == "safe-area") {
        visit(std::type_identity<SafeAreaPage>{});
    } else if (name == "form") {
        visit(std::type_identity<FormPage>{});
    } else if (name == "dash") {
        visit(std::type_identity<DashPage>{});
    } else if (name == "swatch") {
        visit(std::type_identity<SwatchPage>{});
    } else if (name == "icons") {
        visit(std::type_identity<IconsPage>{});
    } else if (name == "lists") {
        visit(std::type_identity<ListsPage>{});
    } else if (name == "controls") {
        visit(std::type_identity<ControlsPage>{});
    } else if (name == "tables") {
        visit(std::type_identity<TablesPage>{});
    } else {
        return false;
    }
    return true;
}

// Runs the named page through `runner`. `Runner` must be callable as `runner(Scene<Page>&)`: it is
// the capture-or-window lambda in `main.cpp`, which captures the parsed arguments by reference, so
// it is a template parameter rather than a `std::function`. The scene lives only for the call.
//
// A name that is not a page returns `std::nullopt`; the caller reports the diagnostic before it
// gets here, so this only has to not guess.
template<typename Runner>
[[nodiscard]] std::optional<int> run_page(std::string_view name, const Runner& runner) {
    std::optional<int> result;
    const bool known = visit_page(name, [&](auto page) {
        Scene<typename decltype(page)::type> scene;
        result = runner(scene);
    });
    return known ? result : std::nullopt;
}

// A scene that outlives the call, for a driver that does not own the loop (the web build: the
// browser calls back into the app long after `main` returned, issue #1023).
class AnyScene {
public:
    virtual ~AnyScene() = default;
    [[nodiscard]] virtual tessera::ui::Node& root() = 0;
    [[nodiscard]] virtual tessera::ui::UpdateScheduler& scheduler() = 0;
};

template<typename Page> class PageScene final : public AnyScene {
public:
    [[nodiscard]] tessera::ui::Node& root() override {
        return scene_.root;
    }
    [[nodiscard]] tessera::ui::UpdateScheduler& scheduler() override {
        return scene_.scheduler;
    }

private:
    Scene<Page> scene_;
};

// The named page in a heap scene, or null for a name that is not a page.
[[nodiscard]] inline std::unique_ptr<AnyScene> make_scene(std::string_view name) {
    std::unique_ptr<AnyScene> scene;
    (void)visit_page(name, [&](auto page) {
        scene = std::make_unique<PageScene<typename decltype(page)::type>>();
    });
    return scene;
}

} // namespace gallery
