// SPDX-License-Identifier: MIT

#include "emit_dirty.hpp"
#include "emit_observables.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const ir::Service* find_service(const ir::Component& component, const std::string& id) {
    const auto found = std::find_if(component.services.begin(), component.services.end(),
                                    [&](const ir::Service& service) { return service.id == id; });
    return found == component.services.end() ? nullptr : &*found;
}

std::string observable_type(const std::string& cpp) {
    return "tessera::appfw::Observable<" + cpp + ">";
}

bool resolve(const ir::Component& component, const ir::IrModule& module,
             const ir::Observable& observable, std::string& cpp, std::string& error) {
    const std::optional<std::string> mapped = cpp_type(observable.type, module.types);
    if (!mapped) {
        return fail(error, "observable `" + observable.id + "` has a type with no C++ form (`" +
                               observable.type + "`)");
    }
    if (find_service(component, observable.service) == nullptr) {
        return fail(error, "observable `" + observable.id + "` reads service `" +
                               observable.service + "`, which the component does not declare");
    }
    cpp = *mapped;
    return true;
}

} // namespace

bool uses_observables(const ir::Component& component) {
    return !component.observables.empty();
}

std::string observable_stem(const ir::Observable& observable) {
    return member_name(observable.id);
}

bool emit_observable_fields(CppWriter& writer, const ir::Component& component,
                            const ir::IrModule& module, std::string& error) {
    for (const ir::Observable& observable : component.observables) {
        std::string cpp;
        if (!resolve(component, module, observable, cpp, error)) {
            return false;
        }
        const std::string stem = observable_stem(observable);
        writer.line("// `" + observable.id + "` (" + observable.service + "." + observable.member +
                    "): the address cached at attach_services(), and the mount-time subscription.");
        writer.line(observable_type(cpp) + "* " + stem + " = nullptr;");
        writer.line(observable_type(cpp) + "::Subscription " + stem + "sub_;");
    }
    return true;
}

std::string observable_accessor_name(const ir::Observable& observable) {
    return observable_stem(observable) + "value";
}

bool emit_observable_accessors(CppWriter& writer, const ir::Component& component,
                               const ir::IrModule& module, std::string& error) {
    for (const ir::Observable& observable : component.observables) {
        std::string cpp;
        if (!resolve(component, module, observable, cpp, error)) {
            return false;
        }
        const std::string stem = observable_stem(observable);
        writer.line("// The current value of `" + observable.id +
                    "`, read in place (no copy); the zero value before attach.");
        writer.line("[[nodiscard]] const " + cpp + "& " + stem + "value() const {");
        writer.line("    static const " + cpp + " kUnattached{};");
        writer.line("    return " + stem + " != nullptr ? " + stem + "->get() : kUnattached;");
        writer.line("}");
    }
    return true;
}

void emit_observable_attach(CppWriter& writer, const ir::Component& component) {
    for (const ir::Observable& observable : component.observables) {
        const ir::Service* service = find_service(component, observable.service);
        if (service == nullptr) {
            continue; // resolve() reported it when the fields were emitted
        }
        writer.line(observable_stem(observable) + " = &std::get<" + service->type +
                    "*>(services_)->" + observable.member + "();");
    }
}

void emit_observable_mount(CppWriter& writer, const ir::Component& component,
                           const ir::IrModule& module) {
    if (!uses_observables(component)) {
        return;
    }
    writer.line("// ADR-050 open question 6: subscribe once; a change marks the observable's bit");
    writer.line("// and schedules this component, so many sets in a tick cost one update().");
    writer.line("using Owner = std::remove_pointer_t<decltype(this)>;");
    for (const ir::Observable& observable : component.observables) {
        const std::optional<std::string> cpp = cpp_type(observable.type, module.types);
        if (!cpp) {
            continue;
        }
        const std::string stem = observable_stem(observable);
        writer.line("if (" + stem + " != nullptr) {");
        ++writer.indent;
        writer.line(stem + "sub_ = " + stem + "->subscribe(");
        writer.line("    [](void* context, const " + *cpp + "&) {");
        writer.line("        auto* owner = static_cast<Owner*>(context);");
        writer.line("        owner->note_dirty(Owner::" + dirty_bit_name(observable.id) + ");");
        writer.line("        owner->schedule_update();");
        writer.line("    },");
        writer.line("    this);");
        --writer.indent;
        writer.line("}");
    }
}

void emit_observable_unmount(CppWriter& writer, const ir::Component& component) {
    for (std::size_t i = component.observables.size(); i-- > 0;) {
        writer.line(observable_stem(component.observables[i]) + "sub_.unsubscribe();");
    }
}

} // namespace tessera::compiler::codegen::internal
