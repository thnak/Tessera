// SPDX-License-Identifier: MIT
//
// The gallery page table (issue #528).
//
// The assertion that matters is drift: `kPageNames` is what `--page` validates against and what the
// usage text prints, while `run_page` is what actually builds a page. Two lists means they can
// disagree, so a name with no branch (a page that validates and then does nothing) or a branch with
// no name (a page nothing can reach) is the failure this test exists to catch. Everything else here
// is the small set of facts the table promises.

#include "page_table.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// A stand-in for `main.cpp`'s capture-or-window lambda: it counts the pages it was handed, so a
// branch that builds nothing is visible as a run that never happened.
struct CountingRunner {
    // `mutable` because `run_page` takes the runner by const reference - the real runner captures
    // the parsed arguments by reference and is const-callable, so the table must not require more.
    mutable int calls = 0;
    int operator()(auto& scene) const {
        ++calls;
        // The scene must be a real page in a real root, or the table handed back something empty.
        if (scene.root.id() != gallery::kRootId) {
            return -1;
        }
        return 0;
    }
};

void test_every_name_has_a_branch() {
    // The drift check. A name in `kPageNames` that `run_page` does not handle would validate and
    // then return nullopt, which `main` turns into exit code 3 - a page that "exists" and does not
    // run.
    for (const std::string_view name : gallery::kPageNames) {
        CountingRunner runner;
        const std::optional<int> result = gallery::run_page(name, runner);
        check(result.has_value(), std::string("`") + std::string(name) + "` builds a page");
        check(runner.calls == 1, std::string("`") + std::string(name) + "` runs its scene once");
        check(result.value_or(-1) == 0, std::string("`") + std::string(name) + "` reports success");
    }
}

void test_an_unknown_name_is_refused_by_both_lists() {
    CountingRunner runner;
    check(!gallery::is_page("nope"), "`nope` is not a page");
    check(!gallery::run_page("nope", runner).has_value(),
          "and `run_page` refuses it rather than guessing");
    check(runner.calls == 0, "without building anything");

    // The empty name is the one a missing `--page` value produces, and it must not be a page.
    check(!gallery::is_page(""), "the empty name is not a page");
    check(!gallery::run_page("", runner).has_value(), "and is refused");
}

void test_only_the_form_page_has_a_module() {
    // `--emit-module-css` refuses a page without one (issue #463): the form's scoped stylesheet is
    // the only CSS module in the gallery, so emitting it under another page's name would be a lie.
    check(gallery::page_has_module_css("form"), "the form page has a CSS module");
    for (const std::string_view name : gallery::kPageNames) {
        if (name == "form") {
            continue;
        }
        check(!gallery::page_has_module_css(name),
              std::string("`") + std::string(name) + "` has no CSS module");
    }
}

void test_the_default_page_is_first() {
    // `--page`'s default is `buttons`, and the usage text lists the names in table order, so the
    // default being first is what makes the help read sensibly. It is also what `main` relies on
    // when it leaves `args.page` at its parsed default.
    check(!gallery::kPageNames.empty(), "there is at least one page");
    check(gallery::kPageNames.front() == "buttons", "`buttons` is the first (and default) page");
    check(gallery::is_page("buttons"), "and it is a page");
}

} // namespace

int main() {
    test_every_name_has_a_branch();
    test_an_unknown_name_is_refused_by_both_lists();
    test_only_the_form_page_has_a_module();
    test_the_default_page_is_first();
    if (failures != 0) {
        std::cerr << "gallery_page_table: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "gallery_page_table: ok\n";
    return 0;
}
