// SPDX-License-Identifier: MIT

#include "form_module.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

namespace gallery {
namespace {

bool write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

} // namespace

std::optional<FormModule> load_form_module(std::string_view text) {
    namespace css = tessera::compiler::css_module;
    const auto scanned = css::scan_module_css(kFormModulePath, text);
    if (!scanned) {
        std::cerr << "gallery: " << kFormModulePath << ": " << scanned.error().code << ": "
                  << scanned.error().message << "\n";
        return std::nullopt;
    }
    FormModule module;
    module.css = scanned->rewritten;
    for (const css::ModuleClass& item : scanned->classes) {
        module.classes.emplace(item.local, item.scoped);
    }
    return module;
}

std::string scoped_class(const FormModule& module, std::string_view local) {
    const auto found = module.classes.find(local);
    return found == module.classes.end() ? std::string(local) : found->second;
}

std::string scope_click_spec(const FormModule& module, std::string_view spec) {
    const std::size_t colon = spec.find(':');
    const std::string_view name = spec.substr(0, colon);
    std::string scoped = scoped_class(module, name);
    if (colon != std::string_view::npos) {
        scoped += spec.substr(colon);
    }
    return scoped;
}

bool write_parity_twin(const FormModule& module, const std::string& dir) {
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    std::string json = "{\n";
    bool first = true;
    for (const auto& [local, scoped] : module.classes) {
        json += first ? "" : ",\n";
        json += "  \"" + local + "\": \"" + scoped + "\"";
        first = false;
    }
    json += "\n}\n";
    const std::filesystem::path base(dir);
    return write_text(base / "form.scoped.css", module.css) &&
           write_text(base / "form.classes.json", json);
}

} // namespace gallery
