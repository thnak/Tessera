// SPDX-License-Identifier: MIT

#include "shim_table.hpp"

#include <iterator>

namespace tessera::compiler::jsx::shim {
namespace {

using enum Status;

constexpr std::string_view kRouter = "react-router-dom";
constexpr std::string_view kRouterCore = "react-router";
constexpr std::string_view kZustand = "zustand";
constexpr std::string_view kReactI18next = "react-i18next";
constexpr std::string_view kI18next = "i18next";

constexpr std::string_view kRootHint =
    "there is no router component: the app root provides the NavController (navigation-v1.md); "
    "delete the wrapper";

// `react-router` is the same table as `react-router-dom`; `find_row` folds the two.
constexpr Row kRows[] = {
    {kRouter, "useNavigate", Supported, "const navigate = useNavigate(); navigate(\"/path\")"},
    {kRouter, "Routes", Pending,
     "declare the table in C++ (RouteTable) until the Routes lowering lands (#1044)"},
    {kRouter, "Route", Pending,
     "declare the table in C++ (RouteTable) until the Routes lowering lands (#1044)"},
    {kRouter, "Outlet", Pending, "nested layouts land with the Routes lowering (#1044)"},
    {kRouter, "Link", Pending,
     "write a View with onClick={() => navigate(\"/path\")} until #542 lands internal href"},
    {kRouter, "NavLink", Pending,
     "write a View with onClick={() => navigate(\"/path\")} until #542 lands internal href"},
    {kRouter, "useParams", Pending,
     "route params are read from the entry after the observable-read lowering (#1044)"},
    {kRouter, "useLocation", Pending, "the location is read from the route observable after #1044"},
    {kRouter, "BrowserRouter", Unsupported, kRootHint},
    {kRouter, "HashRouter", Unsupported, kRootHint},
    {kRouter, "MemoryRouter", Unsupported, kRootHint},
    {kRouter, "RouterProvider", Unsupported, kRootHint},
    {kRouter, "createBrowserRouter", Unsupported, kRootHint},
    {kRouter, "useSearchParams", Unsupported,
     "declare the query value as a typed route param (navigation-v1.md section 1)"},
    {kRouter, "Navigate", Unsupported,
     "call navigate(path, { replace: true }) from an effect or a handler"},
    {kRouter, "useLoaderData", Unsupported,
     "load data with useResource (pending-ui-v1.md), not a route loader"},
    {kRouter, "useNavigation", Unsupported, "read isPending from useTransition (pending-ui-v1.md)"},
    {kRouter, "useBlocker", Unsupported,
     "register a guard on the NavController in C++ (navigation-v1.md section 4)"},
    {kRouter, "Form", Unsupported,
     "use the library Form (forms-and-validation-v1.md), not a routed form"},
    {kRouter, "ScrollRestoration", Unsupported,
     "scroll restoration is the restore snapshot (state-restoration-v1.md)"},

    {kZustand, "create", Pending,
     "keep the state in useState, or read an Observable through a service (store-i18n-shims-v1.md "
     "section 3)"},
    {kZustand, "createStore", Unsupported, "use create; a vanilla store has no TSX reader"},
    {kZustand, "useStore", Unsupported, "use the hook create returned"},
    {kZustand, "shallow", Unsupported,
     "selectors compare by value already (store-i18n-shims-v1.md section 3)"},
    {kZustand, "useShallow", Unsupported,
     "selectors compare by value already (store-i18n-shims-v1.md section 3)"},

    {kReactI18next, "useTranslation", Pending,
     "bind the string with a prop until the t() lowering lands (store-i18n-shims-v1.md section 5)"},
    {kReactI18next, "Trans", Unsupported,
     "write the message with t() and interpolation values; rich-text children have no form"},
    {kReactI18next, "withTranslation", Unsupported, "use the useTranslation hook"},
    {kReactI18next, "I18nextProvider", Unsupported,
     "the app root provides the LocaleService; delete the wrapper"},
    {kReactI18next, "initReactI18next", Unsupported,
     "configure the catalog on the LocaleService in C++ (i18n-v1.md)"},
    {kI18next, "t", Pending,
     "use useTranslation().t once the t() lowering lands (store-i18n-shims-v1.md section 5)"},
    {kI18next, "changeLanguage", Pending,
     "set the locale on the LocaleService through a service call after #1046"},
    {kI18next, "init", Unsupported,
     "configure the catalog on the LocaleService in C++ (i18n-v1.md)"},
    {kI18next, "use", Unsupported, "plugins are not run: no JS runtime (ADR-024)"},
    {kI18next, "createInstance", Unsupported, "there is one LocaleService per app scope"},
};

constexpr std::string_view fold(std::string_view package) noexcept {
    return package == kRouterCore ? kRouter : package;
}

} // namespace

const Row* find_row(std::string_view package, std::string_view name) noexcept {
    for (const Row& row : kRows) {
        if (row.package == fold(package) && row.name == name) {
            return &row;
        }
    }
    return nullptr;
}

bool is_known_package(std::string_view package) noexcept {
    const std::string_view folded = fold(package);
    return folded == kRouter || folded == kZustand || folded == kReactI18next || folded == kI18next;
}

std::string_view unlisted_hint(std::string_view package) noexcept {
    const std::string_view folded = fold(package);
    if (folded == kRouter) {
        return "see docs/specs/router-shim-v1.md section 3 for the covered names";
    }
    if (folded == kZustand) {
        return "see docs/specs/store-i18n-shims-v1.md section 3 for the covered names";
    }
    return "see docs/specs/store-i18n-shims-v1.md section 5 for the covered names";
}

} // namespace tessera::compiler::jsx::shim
