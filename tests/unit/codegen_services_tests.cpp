// SPDX-License-Identifier: MIT

// ADR-050 section 3.1 (AF5, issue #177): the `useService<T>()` lowering of the ADR-040 generated
// base -- shape, golden, determinism, and the two properties the acceptance names.
//
// The golden is deliberate: `codegen_services_tests --write` rewrites
// `tests/golden/codegen/services/Inbox.g.hpp`. It is a *new* directory, so no existing golden moves
// (proved by `git status tests/golden`: only these two files are added).
//
// The compile side (a developer class deriving from the base, the negative undeclared-service case)
// lives in codegen_services_runtime_tests.cpp and the codegen_base_services_* targets.

#include "codegen_services_module.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef TESSERA_CODEGEN_GOLDEN_DIR
#error "TESSERA_CODEGEN_GOLDEN_DIR must be defined by the build"
#endif

namespace {

namespace codegen = tessera::compiler::codegen;
using tessera::test::codegen_services::make_mixed_module;
using tessera::test::codegen_services::make_services_module;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

const std::string& generated_text(const codegen::CodegenResult& result, std::string_view path,
                                  std::string& storage) {
    storage.clear();
    if (!result) {
        return storage;
    }
    for (const codegen::GeneratedFile& file : *result) {
        if (file.path == path) {
            storage = file.text;
        }
    }
    return storage;
}

void test_shape() {
    const auto result = codegen::generate(make_services_module(), {}, std::nullopt);
    check(result.has_value(), "the services fixture generates in base mode");
    if (!result) {
        std::cerr << "  error: " << result.error().front() << "\n";
        return;
    }
    check(result->size() == 1, "one header-only file per component");
    if (result->empty()) {
        return;
    }
    const std::string& text = result->front().text;
    check(result->front().path == "Inbox.g.hpp", "deterministic path");

    // The accessor surface a lowered `useService<T>()` use goes through.
    check(contains(text, "template <class T>") && contains(text, "T& service() const"),
          "the base exposes the typed service<T>() accessor");
    check(contains(text, "[[nodiscard]] demo::Navigator& nav() const { return "
                         "service<demo::Navigator>(); }"),
          "one accessor per requirement, named after the TSX binding");
    check(contains(text, "[[nodiscard]] demo::MailStore& store() const"),
          "the second requirement gets its accessor too");

    // Resolution: one typed slot, resolved once, and the context dropped.
    check(contains(text, "std::tuple<demo::Navigator*, demo::MailStore*> services_ = {};"),
          "the requirements live in one typed slot tuple");
    check(contains(text, "void attach_services(const tessera::appfw::ServiceContext& context)"),
          "attach_services takes the frozen registry");
    check(contains(text, "&context.get<demo::Navigator>()") &&
              contains(text, "&context.get<demo::MailStore>()") &&
              contains(text, "void mount(const tessera::appfw::ServiceContext& context)"),
          "every requirement is resolved through ServiceContext::get at mount");
    check(!contains(text, "services_ = context") && !contains(text, "const ServiceContext* "),
          "the context itself is never retained (ADR-050 section 3 item 5)");

    // The TSX headers the interface types come from, and the runtime seam.
    check(contains(text, "#include \"tessera/appfw/service_context.hpp\""),
          "the base includes the ServiceContext seam");
    check(contains(text, "#include \"demo/services.hpp\""),
          "the base includes the project header that declares its interfaces");
    check(contains(text, "#include <tuple>") || contains(text, "std::tuple<"),
          "the slot type is reachable");

    // The acceptance: an undeclared service is a named, stable diagnostic, not a template dump.
    check(contains(text, "TESSERA_CG_SERVICE_UNDECLARED"),
          "an undeclared service carries a stable code");
    check(contains(text, "`Inbox`") && contains(text, "`demo::Navigator`") &&
              contains(text, "ui/Inbox.tsx:3:15"),
          "the diagnostic names the component and the declared requirements with their locations");
    check(contains(text, "TESSERA_CG_SERVICE_NOT_INTERFACE"),
          "a non-interface type has its own code");
}

// The control case: a component without `useService` generates no service code, which is what keeps
// every pre-existing golden byte-identical.
void test_without_services_is_unchanged() {
    const auto result = codegen::generate(make_mixed_module(), {}, std::nullopt);
    check(result.has_value(), "the mixed module generates");
    if (!result) {
        return;
    }
    std::string inbox;
    std::string plain;
    generated_text(result, "Inbox.g.hpp", inbox);
    generated_text(result, "Plain.g.hpp", plain);
    check(!inbox.empty() && !plain.empty(), "both components are emitted");
    check(contains(inbox, "service<demo::Navigator>()"),
          "the component with requirements gets the service surface");
    check(!contains(plain, "service<") && !contains(plain, "attach_services") &&
              !contains(plain, "appfw"),
          "a component without requirements gets no service code at all");
}

// ADR-050 section 3.1 item 2: a duplicate requirement is a codegen error, not a generated-C++
// error. Two requirements on the same interface would also make `std::get<T*>` ambiguous.
void test_duplicate_requirement() {
    auto module = make_services_module();
    module.components.front().services.push_back({"svc::nav2", "nav2", "demo::Navigator",
                                                  "demo/services.hpp",
                                                  module.components.front().source});
    const auto result = codegen::generate(module, {}, std::nullopt);
    check(!result.has_value(), "a duplicated service interface is rejected");
    if (!result) {
        check(result.error().front().find("TESSERA_CODEGEN_UNSUPPORTED") != std::string::npos &&
                  result.error().front().find("demo::Navigator") != std::string::npos,
              "the duplicate diagnostic names the interface");
    }
}

void test_determinism() {
    const auto first = codegen::generate(make_services_module(), {}, std::nullopt);
    const auto second = codegen::generate(make_services_module(), {}, std::nullopt);
    check(first && second && first->front().text == second->front().text,
          "service output is byte-identical across runs");
}

void test_golden(bool update) {
    const auto result = codegen::generate(make_services_module(), {}, std::nullopt);
    if (!result || result->empty()) {
        check(false, "service golden needs a generated file");
        return;
    }
    const std::filesystem::path path =
        std::filesystem::path(TESSERA_CODEGEN_GOLDEN_DIR) / "services" / "Inbox.g.hpp";
    if (update) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << result->front().text;
        return;
    }
    check(read_file(path) == result->front().text,
          "services/Inbox.g.hpp matches its golden; run `codegen_services_tests --write`");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--write") {
        test_golden(true);
        return failures == 0 ? 0 : 1;
    }
    test_shape();
    test_without_services_is_unchanged();
    test_duplicate_requirement();
    test_determinism();
    test_golden(false);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen services tests passed\n";
    return 0;
}
