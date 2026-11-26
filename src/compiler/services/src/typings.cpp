// SPDX-License-Identifier: MIT

#include "tessera/compiler/services/typings.hpp"

namespace tessera::compiler::services {

namespace {

// The inner spelling of `wrapper<...>`, or empty when `type` is not that wrapper.
std::string_view inner_of(std::string_view type, std::string_view wrapper) {
    if (type.size() <= wrapper.size() + 2 || !type.starts_with(wrapper) ||
        type[wrapper.size()] != '<' || type.back() != '>') {
        return {};
    }
    return type.substr(wrapper.size() + 1, type.size() - wrapper.size() - 2);
}

} // namespace

std::string manifest_type_to_ts(std::string_view type) {
    if (type == "bool") {
        return "boolean";
    }
    if (type == "int" || type == "float") {
        return "number";
    }
    if (type == "string" || type == "color" || type == "length") {
        return "string";
    }
    if (const std::string_view inner = inner_of(type, "optional"); !inner.empty()) {
        return manifest_type_to_ts(inner) + " | null";
    }
    if (const std::string_view inner = inner_of(type, "list"); !inner.empty()) {
        const std::string element = manifest_type_to_ts(inner);
        // `(T | null)[]`: the union needs parentheses to bind to the array.
        return element.find(' ') == std::string::npos ? element + "[]" : "(" + element + ")[]";
    }
    return std::string(type); // `void` or a record name
}

std::string write_services_typings(const ServicesManifest& manifest) {
    std::string out =
        "// Generated from ui/services.json by tessera_codegen. Do not edit.\n"
        "// Types the services TSX may name in useService<T>() and the methods it may call.\n";
    for (const ManifestService& service : manifest.services) {
        out += "\nexport interface " + service.name + " {\n";
        for (const ManifestMethod& method : service.methods) {
            out += "  " + method.name + "(";
            for (std::size_t i = 0; i < method.params.size(); ++i) {
                out += (i == 0 ? "" : ", ") + method.params[i].name + ": " +
                       manifest_type_to_ts(method.params[i].type);
            }
            out += "): " + manifest_type_to_ts(method.returns) + ";\n";
        }
        out += "}\n";
    }
    if (!manifest.services.empty()) {
        out += "\n";
    }
    for (const ManifestService& service : manifest.services) {
        out += "export declare function useService<T extends " + service.name +
               ">(): " + service.name + ";\n";
    }
    return out;
}

} // namespace tessera::compiler::services
