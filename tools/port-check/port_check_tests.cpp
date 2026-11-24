// SPDX-License-Identifier: MIT
//
// `tessera port-check` (issue #546, ADR-058 section 5 item 1).
//
// The assertions are about the two things the tool is for: that a dependency lands in the class the
// ADR gives it, and that a class-D dependency is never reported without saying what to write
// instead. The first is why a port author runs it; the second is ADR-058 section 1's rule that a
// class-D diagnostic names its replacement and is "never a silent drop".

#include "class_table.hpp"
#include "port_check.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::port_check::PortClass;
using tessera::port_check::PortReport;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// A manifest with the three sections a real project has, so the reader is exercised on the shape it
// will meet rather than on a minimal object.
constexpr std::string_view kManifest = R"({
  "name": "example-port",
  "private": true,
  "scripts": { "dev": "vite", "build": "vite build" },
  "dependencies": {
    "react": "^18.2.0",
    "react-dom": "^18.2.0",
    "react-router-dom": "^6.20.0",
    "@mui/material": "^5.14.0",
    "some-unknown-lib": "^1.0.0"
  },
  "devDependencies": {
    "vite": "^5.0.0",
    "@vitejs/plugin-react": "^4.0.0"
  }
})";

const PortReport& report_for(const std::string& text, PortReport& storage) {
    storage = tessera::port_check::check_manifest("example-port", text);
    return storage;
}

const tessera::port_check::DependencyReport* find(const PortReport& report, std::string_view name) {
    for (const auto& dependency : report.dependencies) {
        if (dependency.name == name) {
            return &dependency;
        }
    }
    return nullptr;
}

void test_each_class_lands_where_the_adr_says() {
    PortReport report;
    (void)report_for(std::string(kManifest), report);
    check(report.error.empty(), "the example manifest parses");

    const auto* react = find(report, "react");
    check(react != nullptr, "react is classified");
    check(react != nullptr && react->port_class == PortClass::SourceCompatible,
          "react is class A: it is an alias of `tessera`");

    const auto* router = find(report, "react-router-dom");
    check(router != nullptr && router->port_class == PortClass::Shim,
          "react-router-dom is class B: a shim provides it");

    const auto* vite = find(report, "vite");
    check(vite != nullptr && vite->port_class == PortClass::BuildTime,
          "vite is class C: the codegen CLI is the build");
    check(vite != nullptr && vite->is_dev, "and the report says it came from devDependencies");

    const auto* mui = find(report, "@mui/material");
    check(mui != nullptr && mui->port_class == PortClass::NotPorted, "@mui/material is class D");
    check(mui != nullptr && !mui->replacement.empty(),
          "and it names the replacement rather than only refusing");

    const auto* unknown = find(report, "some-unknown-lib");
    check(unknown != nullptr && unknown->port_class == PortClass::NotPorted,
          "an unknown package falls back to class D, never to an optimistic guess");
    check(unknown != nullptr && !unknown->replacement.empty(),
          "and the fallback names what to check rather than only refusing");
}

void test_counts_and_blockers_agree_with_the_list() {
    PortReport report;
    (void)report_for(std::string(kManifest), report);
    std::size_t total = 0;
    for (const std::size_t count : report.per_class) {
        total += count;
    }
    check(total == report.dependencies.size(), "the per-class counts cover every dependency");
    check(report.per_class[0] == 1, "one class-A dependency");
    // The manifest's class-D set: @mui/material (no equivalent), some-unknown-lib (the wildcard
    // fallback) and @vitejs/plugin-react (a build plugin no rule names - the same wildcard). Three,
    // not two: an unclassified devDependency is class D by ADR-058 section 4, and counting it is
    // the honest answer rather than quietly exempting devDependencies.
    check(report.per_class[3] == 3, "three class-D dependencies");
    check(report.blockers.size() == report.per_class[3],
          "the blockers are exactly the class-D dependencies");
    check(tessera::port_check::exit_code(report) == 1, "a blocked port exits 1");
}

void test_a_portable_project_exits_zero() {
    constexpr std::string_view kClean = R"({
      "dependencies": { "react": "^18.0.0", "react-router-dom": "^6.0.0" }
    })";
    PortReport report;
    (void)report_for(std::string(kClean), report);
    check(report.error.empty(), "a clean manifest parses");
    check(report.blockers.empty(), "and has no blockers");
    check(tessera::port_check::exit_code(report) == 0, "so the CLI exits 0");
}

void test_a_missing_section_is_not_an_error() {
    constexpr std::string_view kNoDev = R"({ "dependencies": { "react": "^18.0.0" } })";
    PortReport report;
    (void)report_for(std::string(kNoDev), report);
    check(report.error.empty(), "a project with no devDependencies is ordinary");
    check(report.dependencies.size() == 1, "and still reports its runtime dependency");
}

void test_a_manifest_that_cannot_be_used_says_so() {
    PortReport not_an_object;
    (void)report_for("[1, 2, 3]", not_an_object);
    check(!not_an_object.error.empty(), "a non-object package.json is an error");
    check(tessera::port_check::exit_code(not_an_object) == 2, "and exits 2, not 1");

    // A dependencies key that is not an object is the shape a hand-edited manifest gets wrong; an
    // empty report would read as "nothing to port", which is the silent answer the ADR forbids.
    PortReport bad_section;
    (void)report_for(R"({ "dependencies": "react" })", bad_section);
    check(!bad_section.error.empty(), "a dependencies section that is not an object is an error");
}

void test_the_script_field_is_ignored() {
    // `scripts` is an object of strings; a reader that walked every object would report its keys as
    // dependencies.
    PortReport report;
    (void)report_for(std::string(kManifest), report);
    check(find(report, "dev") == nullptr, "a scripts key is not a dependency");
    check(find(report, "build") == nullptr, "and neither is the other one");
}

void test_the_text_and_json_renderings_carry_the_same_verdict() {
    PortReport report;
    (void)report_for(std::string(kManifest), report);
    const std::string text = tessera::port_check::render_text(report);
    const std::string json = tessera::port_check::render_json(report);
    check(text.find("blockers:") != std::string::npos, "the text report lists the blockers");
    check(json.find("\"portCheckVersion\":1") != std::string::npos, "the JSON report is versioned");
    check(json.find("\"D\":3") != std::string::npos, "and carries the same class-D count");
}

void test_the_class_table_has_no_silent_class_d() {
    // ADR-058 section 1: a class-D row names its replacement. This is the rule the table itself
    // must keep, not only the report.
    for (const auto& row : tessera::port_check::class_rows()) {
        const bool blocked = row.port_class == PortClass::NotPorted;
        check(!blocked || !row.replacement.empty(),
              std::string("class-D row `") + std::string(row.package) + "` names a replacement");
        check(!row.source.empty(),
              std::string("row `") + std::string(row.package) + "` cites its rule");
    }
}

} // namespace

int main() {
    test_each_class_lands_where_the_adr_says();
    test_counts_and_blockers_agree_with_the_list();
    test_a_portable_project_exits_zero();
    test_a_missing_section_is_not_an_error();
    test_a_manifest_that_cannot_be_used_says_so();
    test_the_script_field_is_ignored();
    test_the_text_and_json_renderings_carry_the_same_verdict();
    test_the_class_table_has_no_silent_class_d();
    if (failures != 0) {
        std::cerr << "port_check: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "port_check: ok\n";
    return 0;
}
