// SPDX-License-Identifier: MIT

// ADR-050 section 3.1 (AF5): the `useService<T>()` lowering of the generated base.
//
// One owner for the whole service surface of `<Component>.g.hpp`, split in two because the ADR-040
// reshaping partitions the struct body by member shape (a name ending in `_` is a private field,
// everything else is public API):
//
//   emit_service_fields (private)
//     `std::tuple<<Interface>*...> services_ = {};` -- one typed slot per declared requirement,
//     and `has_service_v<T>`, the compile-time predicate the accessor asserts on.
//
//   emit_service_accessors (public)
//     `attach_services(const ServiceContext&)`, which resolves every requirement exactly once and
//     keeps only the typed pointers. The context is not retained, so no later lookup is possible
//     (ADR-050 section 3 items 4 and 5).
//     `mount(const ServiceContext&)`: attach, then the ordinary `mount()`. ADR-050 section 3.1
//     item 3 says resolution happens at mount, and this overload is that statement.
//     `service<T>()`, the typed accessor a lowered `useService<T>()` use reads. It is a
//     cached-pointer read, never a registry lookup, so "never in the frame path" is structural
//     rather than a review rule.
//     `<name>()`, the per-requirement accessor named after the TSX binding, following the ADR-040
//     accessor convention every other generated member uses (`count()`, `title()`, `ref_box()`).
//
// The diagnostic is a `static_assert` carrying a `TESSERA_CG_*` code, the component and the
// declared requirements -- the same convention as `TESSERA_CG_MISSING_BINDING` (emit_base.cpp). It
// lives in a nested `ServiceRequirement<T>` struct instantiated by `service<T>()` rather than in
// `build()`, because it is a property of one call, not of the component's lifetime: a component
// that declares a requirement but never reads it must still compile. The struct is what makes the
// compiler's instantiation trace name the offending type (`ServiceRequirement<demo::Settings>`)
// next to the message, so the diagnostic names the type as well as the component without needing
// reflection.
//
// Emission is conditional on `component.services` being non-empty, so a component that does not use
// `useService` generates not one byte of service code. That is what keeps the committed `.g.hpp`
// goldens byte-identical (AGENTS.md: a golden moves only for a real change).

#include "emit_base_accessors.hpp"
#include "emit_observables.hpp"
#include "emit_resources.hpp"
#include "emit_services.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

// The requirement list the diagnostic prints: `` `demo::Navigator` (ui/Inbox.tsx:3:15) ``.
std::string declared_list(const ir::Component& component) {
    std::string list;
    for (const ir::Service& service : component.services) {
        if (!list.empty()) {
            list += ", ";
        }
        list += "`" + service.type + "` (" + service.source.file + ":" +
                std::to_string(service.source.line) + ":" + std::to_string(service.source.column) +
                ")";
    }
    return list;
}

// `demo::Navigator*, demo::MailStore*` for the slot declaration.
std::string pointer_list(const ir::Component& component) {
    std::string types;
    for (const ir::Service& service : component.services) {
        if (!types.empty()) {
            types += ", ";
        }
        types += service.type + "*";
    }
    return types;
}

// The names this emitter adds. All of them are claimed up front, so a TSX binding that happens to
// use one is a collision diagnostic instead of a redeclaration error in the generated C++.
constexpr std::string_view kGeneratedNames[] = {
    "attach_services", "services_",           "has_service_v",    "ServiceRequirement",
    "service",         "service_unavailable", "required_services"};

// One slot, one pointer. Two requirements on the same C++ interface would make `std::get<T*>`
// ambiguous, and two with the same binding name would collide in the struct, so both are rejected
// here with the component named instead of surfacing as generated-C++ errors.
bool check_requirements(const ir::Component& component, std::string& error) {
    for (std::size_t i = 0; i < component.services.size(); ++i) {
        const ir::Service& service = component.services[i];
        for (std::size_t j = 0; j < i; ++j) {
            const ir::Service& earlier = component.services[j];
            if (earlier.type == service.type) {
                return fail(error, "component `" + component.name + "` declares service `" +
                                       service.type + "` twice");
            }
            if (earlier.name == service.name) {
                return fail(error, "component `" + component.name +
                                       "` binds two services to the same name `" + service.name +
                                       "`");
            }
        }
    }
    return true;
}

bool claim_names(const ir::Component& component, std::set<std::string>& names, std::string& error) {
    const auto claim = [&](const std::string& name) {
        if (reserved_member(name) || !names.insert(name).second) {
            return fail(error, "component `" + component.name + "`: accessor `" + name +
                                   "` collides with another generated member");
        }
        return true;
    };
    for (const std::string_view name : kGeneratedNames) {
        if (!claim(std::string(name))) {
            return false;
        }
    }
    for (const ir::Service& service : component.services) {
        if (!claim(service.name)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool uses_services(const ir::Component& component) {
    return !component.services.empty();
}

std::vector<std::string> service_headers(const ir::Component& component) {
    std::vector<std::string> headers;
    for (const ir::Service& service : component.services) {
        if (!service.header.empty() &&
            std::find(headers.begin(), headers.end(), service.header) == headers.end()) {
            headers.push_back(service.header);
        }
    }
    std::sort(headers.begin(), headers.end());
    return headers;
}

bool emit_service_fields(CppWriter& writer, const ir::Component& component, std::string& error) {
    if (!uses_services(component)) {
        return true;
    }
    if (!check_requirements(component, error)) {
        return false;
    }
    writer.line("// ADR-050 section 3.1: the `useService<T>()` requirements. attach_services()");
    writer.line("// resolves each one exactly once and keeps only the typed pointer.");
    writer.line("std::tuple<" + pointer_list(component) + "> services_ = {};");
    return true;
}

bool claim_service_names(const ir::Component& component, std::set<std::string>& names,
                         std::string& error) {
    if (!uses_services(component)) {
        return true;
    }
    return claim_names(component, names, error);
}

bool emit_service_accessors(CppWriter& writer, const ir::Component& component,
                            std::string& /*error*/) {
    if (!uses_services(component)) {
        return true;
    }
    const std::string pointers = pointer_list(component);
    writer.line("// The compile-time requirement predicate the service<T>() accessor asserts on.");
    writer.line("template <class T>");
    writer.line("static constexpr bool has_service_v =");
    for (std::size_t i = 0; i < component.services.size(); ++i) {
        const char* const separator = i + 1 == component.services.size() ? ";" : " ||";
        writer.line("    std::is_same_v<T, " + component.services[i].type + ">" + separator);
    }
    // The manifest id is part of the contract: a header whose `kServiceId` is another name would
    // register under an id the manifest never mentions, so the mismatch is caught at build time.
    for (const ir::Service& service : component.services) {
        if (service.registry_id.empty()) {
            continue;
        }
        writer.line("static_assert(" + service.type +
                    "::kServiceId == tessera::appfw::service_id(\"" + service.registry_id + "\"),");
        writer.line("              \"TESSERA_CG_SERVICE_ID_MISMATCH: `" + service.type +
                    "` is declared as `" + service.registry_id +
                    "` in ui/services.json but its kServiceId is another name\");");
    }
    writer.line("// Resolves every declared requirement once. The context is not retained, so a");
    writer.line("// lookup after this call is impossible (ADR-050 section 3 item 5).");
    writer.line("void attach_services(const tessera::appfw::ServiceContext& context) {");
    writer.line("    services_ = std::tuple<" + pointers + ">{");
    for (std::size_t i = 0; i < component.services.size(); ++i) {
        const char* const separator = i + 1 == component.services.size() ? "};" : ",";
        writer.line("        &context.get<" + component.services[i].type + ">()" + separator);
    }
    emit_observable_attach(writer, component);
    if (uses_resources(component)) {
        // A resource whose service is a `ResourceProvider` binds its view as soon as the service is
        // known (pending-ui-v1.md section 10.2).
        writer.line("    pending_sync();");
    }
    writer.line("}");
    writer.line("// ADR-050 section 3.1 item 3: resolve at mount, then run the ordinary mount().");
    writer.line("void mount(const tessera::appfw::ServiceContext& context) {");
    writer.line("    attach_services(context);");
    writer.line("    mount();");
    writer.line("}");
    writer.line(
        "// The check a `useService<T>()` read goes through. Instantiated by service<T>(),");
    writer.line("// so the compiler's instantiation trace names the type that was asked for.");
    writer.line("template <class T>");
    writer.line("struct ServiceRequirement {");
    writer.line("    static_assert(tessera::appfw::ProvidedService<T>,");
    writer.line("                  \"TESSERA_CG_SERVICE_NOT_INTERFACE: component `" +
                component.name +
                "` can only resolve a service interface (a type with `kServiceId`), not this "
                "type\");");
    // Vacuous for a type that already failed the interface check, so one mistake gives one
    // diagnostic instead of a second, misleading "not declared" for the same name.
    writer.line("    static_assert(!tessera::appfw::ProvidedService<T> || has_service_v<T>,");
    writer.line("                  \"TESSERA_CG_SERVICE_UNDECLARED: component `" + component.name +
                "` needs a `useService<T>()` declaration for this service type; declared: " +
                declared_list(component) + "\");");
    writer.line("};");
    writer.line("// Declared and never defined: the discarded branch of service<T>() is only ever");
    writer.line("// instantiated after both static_asserts have already rejected the type.");
    writer.line("template <class T>");
    writer.line("static T& service_unavailable();");
    writer.line("// The typed accessor a lowered `useService<T>()` use reads: a cached pointer,");
    writer.line("// never a registry lookup, so it is legal in render and in event handlers.");
    writer.line("template <class T>");
    writer.line("[[nodiscard]] T& service() const {");
    writer.line("    static_assert(sizeof(ServiceRequirement<T>) > 0);");
    writer.line("    if constexpr (has_service_v<T>) {");
    writer.line("        return *std::get<T*>(services_);");
    writer.line("    } else {");
    writer.line("        return service_unavailable<T>();");
    writer.line("    }");
    writer.line("}");
    writer.line(
        "// ADR-050 section 3.1 item 6: this component's requirements, for the composition");
    writer.line("// root's startup validation (tessera::appfw::find_missing_services).");
    writer.line("[[nodiscard]] static std::span<const tessera::appfw::RequiredService>");
    writer.line("required_services() noexcept {");
    writer.line("    static constexpr tessera::appfw::RequiredService kRequired[] = {");
    for (const ir::Service& service : component.services) {
        writer.line("        {" + service.type + "::kServiceId, \"" + service.type + "\", \"" +
                    component.name + "\", \"" + service.source.file + ":" +
                    std::to_string(service.source.line) + ":" +
                    std::to_string(service.source.column) + "\"},");
    }
    writer.line("    };");
    writer.line("    return kRequired;");
    writer.line("}");
    for (const ir::Service& service : component.services) {
        writer.line("// The service `" + service.id + "` (`" + service.name +
                    "` in the TSX), resolved at attach.");
        writer.line("[[nodiscard]] " + service.type + "& " + service.name +
                    "() const { return service<" + service.type + ">(); }");
    }
    return true;
}

} // namespace tessera::compiler::codegen::internal
