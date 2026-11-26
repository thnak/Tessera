// SPDX-License-Identifier: MIT
//
// The `ui/services.json` reader (docs/specs/services-manifest-v1.md, issue #177): a valid file, and
// one case per diagnostic of section 4 so a code cannot drift to another failure.

#include "tessera/compiler/services/manifest.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace svc = tessera::compiler::services;

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

constexpr std::string_view kValid = R"({
  "version": 1,
  "services": [
    {
      "id": "demo.nav",
      "name": "Navigator",
      "cpp": { "type": "demo::Navigator", "header": "demo/navigator.hpp", "note": "ignored" },
      "methods": [
        { "name": "push", "params": [{ "name": "route", "type": "string" }], "returns": "void" },
        { "name": "depth", "params": [], "returns": "int" },
        { "name": "find", "params": [{ "name": "ids", "type": "list<optional<int>>" }],
          "returns": "Route" }
      ]
    },
    {
      "id": "tessera.http",
      "name": "HttpService",
      "cpp": { "type": "tessera::appfw::HttpService", "header": "tessera/appfw/http_service.hpp" },
      "methods": []
    }
  ]
})";

// Parses `text` and returns the error code, or "" when it parsed.
std::string code_of(std::string_view text) {
    const auto parsed = svc::parse_services_manifest(text);
    return parsed ? std::string() : parsed.error().code;
}

std::string with_service(std::string_view service) {
    return std::string(R"({"version":1,"services":[)") + std::string(service) + "]}";
}

void a_valid_file_round_trips() {
    const auto parsed = svc::parse_services_manifest(kValid);
    check(parsed.has_value(), "a valid manifest parses");
    if (!parsed) {
        return;
    }
    check(parsed->services.size() == 2, "both services are read, in order");
    const svc::ManifestService* nav = parsed->find_name("Navigator");
    check(nav != nullptr && nav->id == "demo.nav" && nav->cpp_type == "demo::Navigator" &&
              nav->header == "demo/navigator.hpp",
          "the C++ spelling and header are copied");
    check(nav != nullptr && nav->methods.size() == 3, "three methods are read");
    const svc::ManifestMethod* push = nav != nullptr ? nav->find_method("push") : nullptr;
    check(push != nullptr && push->returns == "void" && push->params.size() == 1 &&
              push->params[0].name == "route" && push->params[0].type == "string",
          "a method keeps its params and return in order");
    check(nav != nullptr && nav->find_method("find") != nullptr &&
              nav->find_method("find")->returns == "Route",
          "a record name is accepted by shape");
    check(parsed->find_id("tessera.http") != nullptr, "lookup by id works");
    check(parsed->find_name("Missing") == nullptr, "an unknown name finds nothing");
}

void each_diagnostic_has_its_code() {
    check(code_of("{not json") == "TESSERA_SVC_MANIFEST_SYNTAX", "invalid JSON is syntax");
    check(code_of("[]") == "TESSERA_SVC_MANIFEST_SYNTAX", "a non-object root is syntax");
    check(code_of(R"({"version":1})") == "TESSERA_SVC_MANIFEST_SYNTAX",
          "missing services is syntax");
    check(code_of(R"({"version":2,"services":[]})") == "TESSERA_SVC_MANIFEST_VERSION",
          "a newer version is version, and the file is not partly read");
    check(code_of(R"({"version":"1","services":[]})") == "TESSERA_SVC_MANIFEST_SYNTAX",
          "a string version is syntax");
    check(code_of(R"({"version":1,"services":[],"extra":1})") == "TESSERA_SVC_MANIFEST_UNKNOWN_KEY",
          "an unknown top-level key is unknown_key");
    check(code_of(with_service(R"({"id":"a.b","name":"A","cpp":{"type":"A","header":"a.hpp"},)"
                               R"("methods":[],"oops":1})")) == "TESSERA_SVC_MANIFEST_UNKNOWN_KEY",
          "an unknown service key is unknown_key");
    check(code_of(with_service(R"({"id":"Bad Id","name":"A","cpp":{"type":"A","header":"a.hpp"},)"
                               R"("methods":[]})")) == "TESSERA_SVC_MANIFEST_SYNTAX",
          "an id that is not reverse-DNS is syntax");
    check(code_of(with_service(R"({"id":"a.b","name":"1A","cpp":{"type":"A","header":"a.hpp"},)"
                               R"("methods":[]})")) == "TESSERA_SVC_MANIFEST_SYNTAX",
          "a name that is not an identifier is syntax");
}

void types_and_duplicates() {
    const auto method = [](std::string_view body) {
        return with_service(
            std::string(R"({"id":"a.b","name":"A","cpp":{"type":"A","header":"a.hpp"},)"
                        R"("methods":[)") +
            std::string(body) + "]}");
    };
    check(code_of(method(
              R"({"name":"m","params":[{"name":"x","type":"number"}],"returns":"void"})")) == "",
          "an identifier-shaped record name is accepted by shape");
    check(code_of(
              method(R"({"name":"m","params":[{"name":"x","type":"list<>"}],"returns":"void"})")) ==
              "TESSERA_SVC_MANIFEST_TYPE",
          "an empty list element is a type error");
    check(
        code_of(method(R"({"name":"m","params":[{"name":"x","type":"void"}],"returns":"void"})")) ==
            "TESSERA_SVC_MANIFEST_TYPE",
        "void is not a parameter type");
    check(code_of(method(R"({"name":"m","params":[],"returns":"optional<void>"})")) ==
              "TESSERA_SVC_MANIFEST_TYPE",
          "void cannot be wrapped");
    check(code_of(method(
              R"({"name":"m","params":[{"name":"x","type":"callback"}],"returns":"void"})")) ==
              "TESSERA_SVC_MANIFEST_TYPE",
          "an IR-only word such as callback is not a service type");
    check(code_of(method(R"({"name":"m","params":[],"returns":"a b"})")) ==
              "TESSERA_SVC_MANIFEST_TYPE",
          "a spelling with a space is a type error");
    check(code_of(method(R"({"name":"m","params":[],"returns":"void"},)"
                         R"({"name":"m","params":[],"returns":"int"})")) ==
              "TESSERA_SVC_MANIFEST_DUPLICATE",
          "two methods with one name are a duplicate");
    const std::string one =
        R"({"id":"a.b","name":"A","cpp":{"type":"A","header":"a.hpp"},"methods":[]})";
    check(code_of(std::string(R"({"version":1,"services":[)") + one + "," + one + "]}") ==
              "TESSERA_SVC_MANIFEST_DUPLICATE",
          "two services with one id and name are a duplicate");
    const auto bad = svc::parse_services_manifest(
        method(R"({"name":"m","params":[{"name":"x","type":"list<>"}],"returns":"void"})"));
    check(!bad && bad.error().pointer == "/services/0/methods/0/params/0/type",
          "the error carries the JSON pointer of the offending value");
}

} // namespace

int main() {
    a_valid_file_round_trips();
    each_diagnostic_has_its_code();
    types_and_duplicates();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
