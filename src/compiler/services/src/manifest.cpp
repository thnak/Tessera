// SPDX-License-Identifier: MIT

#include "tessera/compiler/services/manifest.hpp"
#include "tessera/harness/json_parse.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>

namespace tessera::compiler::services {
namespace {

using harness::JsonType;
using harness::JsonValue;

bool is_identifier(std::string_view text) {
    if (text.empty() ||
        (std::isalpha(static_cast<unsigned char>(text.front())) == 0 && text.front() != '_')) {
        return false;
    }
    return std::ranges::all_of(text, [](char character) {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    });
}

// `a.b.c`: reverse-DNS, lowercase letters, digits, `-` and `_` inside segments.
bool is_reverse_dns(std::string_view text) {
    if (text.empty() || text.front() == '.' || text.back() == '.') {
        return false;
    }
    char previous = 0;
    for (const char character : text) {
        const bool ok = (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') || character == '-' ||
                        character == '_' || character == '.';
        if (!ok || (character == '.' && previous == '.')) {
            return false;
        }
        previous = character;
    }
    return true;
}

bool is_scalar(std::string_view type) {
    return type == "bool" || type == "int" || type == "float" || type == "string" ||
           type == "color" || type == "length";
}

bool is_wrapped(std::string_view type, std::string_view wrapper, std::string_view& inner) {
    if (!type.starts_with(wrapper) || type.size() < wrapper.size() + 2 ||
        type[wrapper.size()] != '<' || type.back() != '>') {
        return false;
    }
    inner = type.substr(wrapper.size() + 1, type.size() - wrapper.size() - 2);
    return true;
}

// Words the IR gives another meaning: not a value type a service method can take or return.
bool is_reserved(std::string_view type) {
    return type == "void" || type == "children" || type == "callback" || type == "node_ref";
}

bool is_value_type(std::string_view type) {
    std::string_view inner;
    if (is_wrapped(type, "list", inner) || is_wrapped(type, "optional", inner)) {
        return is_value_type(inner);
    }
    return is_scalar(type) || (is_identifier(type) && !is_reserved(type));
}

ManifestError error(std::string code, std::string pointer, std::string message,
                    std::size_t offset = 0) {
    return ManifestError{std::move(code), std::move(pointer), std::move(message), offset};
}

std::string at(const std::string& base, std::string_view key) {
    return base + "/" + std::string(key);
}

std::string at(const std::string& base, std::size_t index) {
    return base + "/" + std::to_string(index);
}

// A required string member; `syntax` error when absent or not a string.
std::expected<std::string, ManifestError>
required_string(const JsonValue& object, std::string_view key, const std::string& base) {
    const JsonValue* value = object.find(key);
    if (value == nullptr || !value->is_string()) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, key),
                  "`" + std::string(key) + "` is required and must be a string"));
    }
    return std::string(*value->as_string());
}

std::expected<void, ManifestError> check_keys(const JsonValue& object, const std::string& base,
                                              std::initializer_list<std::string_view> allowed) {
    for (const auto& [key, unused] : *object.as_object()) {
        if (std::ranges::find(allowed, std::string_view(key)) == allowed.end()) {
            return std::unexpected(error("TESSERA_SVC_MANIFEST_UNKNOWN_KEY", at(base, key),
                                         "unknown key `" + key + "`"));
        }
    }
    return {};
}

std::expected<ManifestMethod, ManifestError> read_method(const JsonValue& node,
                                                         const std::string& base) {
    if (!node.is_object()) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", base, "a method must be an object"));
    }
    if (auto keys = check_keys(node, base, {"name", "params", "returns"}); !keys) {
        return std::unexpected(keys.error());
    }
    ManifestMethod method;
    auto name = required_string(node, "name", base);
    if (!name) {
        return std::unexpected(name.error());
    }
    method.name = *name;
    if (!is_identifier(method.name)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "name"),
                                     "a method name must be an identifier"));
    }
    auto returns = required_string(node, "returns", base);
    if (!returns) {
        return std::unexpected(returns.error());
    }
    method.returns = *returns;
    if (!is_manifest_type(method.returns, true)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_TYPE", at(base, "returns"),
                                     "`" + method.returns + "` is not an IR type or void"));
    }
    const JsonValue* params = node.find("params");
    if (params == nullptr || !params->is_array()) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "params"),
                                     "`params` is required and must be an array"));
    }
    const std::string params_base = at(base, "params");
    std::size_t index = 0;
    for (const JsonValue& param : *params->as_array()) {
        const std::string here = at(params_base, index++);
        if (!param.is_object()) {
            return std::unexpected(
                error("TESSERA_SVC_MANIFEST_SYNTAX", here, "a param must be an object"));
        }
        if (auto keys = check_keys(param, here, {"name", "type"}); !keys) {
            return std::unexpected(keys.error());
        }
        auto param_name = required_string(param, "name", here);
        auto param_type = required_string(param, "type", here);
        if (!param_name) {
            return std::unexpected(param_name.error());
        }
        if (!param_type) {
            return std::unexpected(param_type.error());
        }
        if (!is_identifier(*param_name)) {
            return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(here, "name"),
                                         "a param name must be an identifier"));
        }
        if (!is_manifest_type(*param_type, false)) {
            return std::unexpected(error("TESSERA_SVC_MANIFEST_TYPE", at(here, "type"),
                                         "`" + *param_type + "` is not an IR type"));
        }
        method.params.push_back({*param_name, *param_type});
    }
    return method;
}

std::expected<ManifestObservable, ManifestError> read_observable(const JsonValue& node,
                                                                 const std::string& base) {
    if (!node.is_object()) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", base, "an observable must be an object"));
    }
    if (auto keys = check_keys(node, base, {"name", "type"}); !keys) {
        return std::unexpected(keys.error());
    }
    auto name = required_string(node, "name", base);
    auto type = required_string(node, "type", base);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!type) {
        return std::unexpected(type.error());
    }
    if (!is_identifier(*name)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "name"),
                                     "an observable name must be an identifier"));
    }
    if (!is_manifest_type(*type, false)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_TYPE", at(base, "type"),
                                     "`" + *type + "` is not an IR type"));
    }
    return ManifestObservable{*name, *type};
}

std::expected<ManifestService, ManifestError> read_service(const JsonValue& node,
                                                           const std::string& base) {
    if (!node.is_object()) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", base, "a service must be an object"));
    }
    if (auto keys = check_keys(node, base, {"id", "name", "cpp", "methods", "observables"});
        !keys) {
        return std::unexpected(keys.error());
    }
    ManifestService service;
    auto id = required_string(node, "id", base);
    auto name = required_string(node, "name", base);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (!name) {
        return std::unexpected(name.error());
    }
    service.id = *id;
    service.name = *name;
    if (!is_reverse_dns(service.id)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "id"),
                                     "`id` must be a reverse-DNS name"));
    }
    if (!is_identifier(service.name)) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "name"),
                                     "`name` must be a TSX identifier"));
    }
    const JsonValue* cpp = node.find("cpp");
    if (cpp == nullptr || !cpp->is_object()) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "cpp"),
                                     "`cpp` is required and must be an object"));
    }
    // Inside `cpp` an unknown key is ignored (spec section 2).
    auto cpp_type = required_string(*cpp, "type", at(base, "cpp"));
    auto header = required_string(*cpp, "header", at(base, "cpp"));
    if (!cpp_type) {
        return std::unexpected(cpp_type.error());
    }
    if (!header) {
        return std::unexpected(header.error());
    }
    service.cpp_type = *cpp_type;
    service.header = *header;
    const JsonValue* methods = node.find("methods");
    if (methods == nullptr || !methods->is_array()) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "methods"),
                                     "`methods` is required and must be an array"));
    }
    std::size_t index = 0;
    for (const JsonValue& entry : *methods->as_array()) {
        auto method = read_method(entry, at(at(base, "methods"), index++));
        if (!method) {
            return std::unexpected(method.error());
        }
        if (service.find_method(method->name) != nullptr) {
            return std::unexpected(
                error("TESSERA_SVC_MANIFEST_DUPLICATE", at(at(base, "methods"), index - 1),
                      "service `" + service.name + "` lists method `" + method->name + "` twice"));
        }
        service.methods.push_back(std::move(*method));
    }
    if (const JsonValue* observables = node.find("observables"); observables != nullptr) {
        if (!observables->is_array()) {
            return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", at(base, "observables"),
                                         "`observables` must be an array"));
        }
        std::size_t position = 0;
        for (const JsonValue& entry : *observables->as_array()) {
            auto observable = read_observable(entry, at(at(base, "observables"), position++));
            if (!observable) {
                return std::unexpected(observable.error());
            }
            // One C++ member name per interface: an observable and a method cannot share it.
            if (service.find_observable(observable->name) != nullptr ||
                service.find_method(observable->name) != nullptr) {
                return std::unexpected(error("TESSERA_SVC_MANIFEST_DUPLICATE",
                                             at(at(base, "observables"), position - 1),
                                             "service `" + service.name + "` lists member `" +
                                                 observable->name + "` twice"));
            }
            service.observables.push_back(std::move(*observable));
        }
    }
    return service;
}

} // namespace

bool is_manifest_type(std::string_view type, bool allow_void) {
    return (allow_void && type == "void") || is_value_type(type);
}

std::expected<ServicesManifest, ManifestError> parse_services_manifest(std::string_view text) {
    const auto parsed = harness::parse_json(text);
    if (!parsed) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", "",
                  "not valid JSON (offset " + std::to_string(parsed.error().offset) + ")",
                  parsed.error().offset));
    }
    if (!parsed->is_object()) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", "", "the manifest must be a JSON object"));
    }
    if (auto keys = check_keys(*parsed, "", {"version", "services"}); !keys) {
        return std::unexpected(keys.error());
    }
    const JsonValue* version = parsed->find("version");
    if (version == nullptr || !version->as_integer().has_value()) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", "/version",
                                     "`version` is required and must be an integer"));
    }
    if (*version->as_integer() != kManifestVersion) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_VERSION", "/version",
                                     "version " + std::to_string(*version->as_integer()) +
                                         " is not supported; this reader reads version 1"));
    }
    const JsonValue* services = parsed->find("services");
    if (services == nullptr || !services->is_array()) {
        return std::unexpected(error("TESSERA_SVC_MANIFEST_SYNTAX", "/services",
                                     "`services` is required and must be an array"));
    }
    ServicesManifest manifest;
    std::size_t index = 0;
    for (const JsonValue& entry : *services->as_array()) {
        const std::string here = at("/services", index++);
        auto service = read_service(entry, here);
        if (!service) {
            return std::unexpected(service.error());
        }
        if (manifest.find_id(service->id) != nullptr ||
            manifest.find_name(service->name) != nullptr) {
            return std::unexpected(error("TESSERA_SVC_MANIFEST_DUPLICATE", here,
                                         "`" + service->name + "` repeats an id or a name"));
        }
        manifest.services.push_back(std::move(*service));
    }
    return manifest;
}

std::expected<std::optional<ServicesManifest>, ManifestError>
load_services_manifest(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "ui" / "services.json";
    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(path, exists_error)) {
        return std::optional<ServicesManifest>{};
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::unexpected(
            error("TESSERA_SVC_MANIFEST_SYNTAX", "", "ui/services.json cannot be read"));
    }
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    auto parsed = parse_services_manifest(text);
    if (!parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return std::optional<ServicesManifest>(std::move(*parsed));
}

} // namespace tessera::compiler::services
