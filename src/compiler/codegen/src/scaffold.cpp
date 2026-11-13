// SPDX-License-Identifier: MIT

// ADR-040 decision item 6: the developer-owned half of a component. `scaffold()` returns the stub
// `<Component>.hpp` / `<Component>.cpp` the CLI writes once and never rewrites, and
// `required_bindings()` lists the members the developer class must define so the CLI can report a
// binding that an existing developer file does not define yet.

#include "emit_base.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen {
namespace {

using internal::BindingShape;

std::vector<const ir::Binding*> bindings_of(const ir::IrModule& module,
                                            const ir::Component& component) {
    std::vector<const ir::Binding*> result;
    for (const ir::Binding& binding : module.bindings) {
        if (binding.component == component.id) {
            result.push_back(&binding);
        }
    }
    return result;
}

std::vector<const ir::Component*> sorted_components(const ir::IrModule& module) {
    std::vector<const ir::Component*> components;
    for (const ir::Component& component : module.components) {
        components.push_back(&component);
    }
    std::sort(components.begin(), components.end(),
              [](const ir::Component* left, const ir::Component* right) {
                  return left->name < right->name;
              });
    return components;
}

std::string location(const ir::Binding& binding) {
    return binding.source.file + ":" + std::to_string(binding.source.line) + ":" +
           std::to_string(binding.source.column);
}

std::string stub_header(const ir::IrModule& module, const ir::Component& component,
                        const std::vector<const ir::Binding*>& bindings, std::string& error) {
    const std::string& name = component.name;
    std::string text;
    text += "// SPDX-License-Identifier: MIT\n";
    text +=
        "// Developer-owned: written once by tessera_codegen and never rewritten. Edit freely.\n";
    text += "// The generated base is " + name + ".g.hpp (regenerated, never edited).\n";
    text += "#pragma once\n\n";
    text += "#include \"" + name + ".g.hpp\"\n\n";
    text += "namespace " + module.name + " {\n\n";
    text += "class " + name + " final : public gen::" + name + "Base<" + name + "> {\n";
    text += "private:\n";
    text += "    friend struct gen::" + name + "Base<" + name + ">;\n";
    for (const ir::Binding* binding : bindings) {
        const std::optional<BindingShape> shape = internal::binding_shape(*binding, error);
        if (!shape) {
            return {};
        }
        text += "\n    // " + binding->id + " (" + location(*binding) + ")\n";
        text += "    " + internal::binding_signature(*binding, *shape) + ";\n";
    }
    text += "};\n\n";
    text += "} // namespace " + module.name + "\n";
    return text;
}

std::string stub_source(const ir::IrModule& module, const ir::Component& component,
                        const std::vector<const ir::Binding*>& bindings, std::string& error) {
    const std::string& name = component.name;
    std::string text;
    text += "// SPDX-License-Identifier: MIT\n\n";
    text += "#include \"" + name + ".hpp\"\n\n";
    text += "namespace " + module.name + " {\n";
    for (const ir::Binding* binding : bindings) {
        const std::optional<BindingShape> shape = internal::binding_shape(*binding, error);
        if (!shape) {
            return {};
        }
        text += "\n// " + binding->id + "\n";
        text += shape->return_type + " " + name + "::" + binding->region + "(" +
                (shape->event ? shape->param_type + " " + shape->param_name : std::string()) +
                ") {\n";
        text += "    // TODO(tessera-codegen): implement " + binding->id + ".\n";
        if (shape->event) {
            text += "    (void)" + shape->param_name + ";\n";
        }
        if (shape->return_type != "void") {
            text += "    return {};\n";
        }
        text += "}\n";
    }
    text += "\n} // namespace " + module.name + "\n";
    return text;
}

} // namespace

std::vector<RequiredBinding> required_bindings(const ir::IrModule& module, std::string& error) {
    std::vector<RequiredBinding> required;
    for (const ir::Component* component : sorted_components(module)) {
        for (const ir::Binding* binding : bindings_of(module, *component)) {
            const std::optional<BindingShape> shape = internal::binding_shape(*binding, error);
            if (!shape) {
                return {};
            }
            required.push_back(RequiredBinding{component->name, binding->id, binding->region,
                                               internal::binding_signature(*binding, *shape),
                                               location(*binding)});
        }
    }
    return required;
}

CodegenResult scaffold(const ir::IrModule& module) {
    std::vector<GeneratedFile> files;
    std::string error;
    for (const ir::Component* component : sorted_components(module)) {
        const std::vector<const ir::Binding*> bindings = bindings_of(module, *component);
        std::string header = stub_header(module, *component, bindings, error);
        std::string source = stub_source(module, *component, bindings, error);
        if (!error.empty()) {
            return std::unexpected(CodegenErrors{error});
        }
        files.push_back(GeneratedFile{component->name + ".hpp", std::move(header)});
        files.push_back(GeneratedFile{component->name + ".cpp", std::move(source)});
    }
    return files;
}

} // namespace tessera::compiler::codegen
