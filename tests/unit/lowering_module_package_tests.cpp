// SPDX-License-Identifier: MIT

// Package resolution for bare specifiers (ADR-014 project config, issue #70): a configured package
// root turns `from "tessera-ui/Button"` into `<root>/Button`, and a bare `from "tessera-ui"` into
// `<root>/index`, so package sources are compiled by the ordinary pipeline. `"tessera"` and
// `"react"` stay built-in and are never package imports.

#include "lowering_module_test_util.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace lowering_module_test;

constexpr std::string_view kButton = "function Button() { return <View />; }\n";
constexpr std::string_view kIndex = "function LibraryRoot() { return <View />; }\n";

lowering::ModuleLoweringResult
lower_with_packages(const Project& project, std::string_view module_name,
                    std::span<const lowering::PackageRoot> packages) {
    const std::vector<lowering::ModuleSource> list = project.sources();
    return lowering::lower_project(list, module_name, packages);
}

void test_subpath_import_resolves_inside_the_package_root() {
    Project project;
    project.add(parse_file("ui/App.tsx", "import { Button } from \"tessera-ui/Button\";\n"
                                         "function App() { return <Button />; }\n"));
    project.add(parse_file("packages/tessera-ui/Button.tsx", std::string(kButton)));

    const std::array<lowering::PackageRoot, 1> packages{
        {{.name = "tessera-ui", .root = "packages/tessera-ui"}}};
    const auto result = lower_with_packages(project, "app", packages);
    check(result.has_value(), "a package subpath import resolves");
    if (!result) {
        check(false, "unexpected error: " + first_message(result));
        return;
    }
    check(find_component(*result, "App") != nullptr, "the importing component is lowered");
    check(find_component(*result, "Button") != nullptr,
          "the package component is compiled by the same pipeline");
}

void test_bare_specifier_resolves_the_package_index() {
    Project project;
    project.add(parse_file("ui/App.tsx", "import { LibraryRoot } from \"tessera-ui\";\n"
                                         "function App() { return <LibraryRoot />; }\n"));
    project.add(parse_file("packages/tessera-ui/index.tsx", std::string(kIndex)));

    const std::array<lowering::PackageRoot, 1> packages{
        {{.name = "tessera-ui", .root = "packages/tessera-ui"}}};
    const auto result = lower_with_packages(project, "app", packages);
    check(result.has_value(), "a bare package specifier resolves the index file");
    if (!result) {
        return;
    }
    check(find_component(*result, "LibraryRoot") != nullptr, "the index component is lowered");
}

void test_unknown_package_is_rejected_and_named() {
    Project project;
    project.add(parse_file("ui/App.tsx", "import { Button } from \"missing-ui/Button\";\n"
                                         "function App() { return <View />; }\n"));

    const auto result = lower_with_packages(project, "app", {});
    check(!result.has_value(), "an unconfigured package is rejected");
    check_eq(first_code(result), "TESSERA_IR_UNSUPPORTED", "the code");
    check(first_message(result).find("missing-ui/Button") != std::string::npos,
          "the diagnostic names the specifier");
}

void test_configured_package_without_the_file_is_unresolved() {
    Project project;
    project.add(parse_file("ui/App.tsx", "import { Card } from \"tessera-ui/Card\";\n"
                                         "function App() { return <Card />; }\n"));
    project.add(parse_file("packages/tessera-ui/Button.tsx", std::string(kButton)));

    const std::array<lowering::PackageRoot, 1> packages{
        {{.name = "tessera-ui", .root = "packages/tessera-ui"}}};
    const auto result = lower_with_packages(project, "app", packages);
    check(!result.has_value(), "a missing package file is an error");
    check_eq(first_code(result), "TESSERA_IR_UNRESOLVED", "the code");
}

void test_framework_specifiers_stay_built_in() {
    // `"tessera"` and `"react"` must not be captured by a package root of the same name.
    Project project;
    project.add(parse_file("ui/App.tsx",
                           "import { useState } from \"react\";\n"
                           "import { View } from \"tessera\";\n"
                           "function App() { const [n, setN] = useState(0); return <View />; }\n"));

    const std::array<lowering::PackageRoot, 1> packages{
        {{.name = "react", .root = "packages/react"}}};
    const auto result = lower_with_packages(project, "app", packages);
    check(result.has_value(), "framework specifiers keep the built-in path");
}

} // namespace

int main() {
    test_subpath_import_resolves_inside_the_package_root();
    test_bare_specifier_resolves_the_package_index();
    test_unknown_package_is_rejected_and_named();
    test_configured_package_without_the_file_is_unresolved();
    test_framework_specifiers_stay_built_in();
    return finish("lowering module package tests");
}
