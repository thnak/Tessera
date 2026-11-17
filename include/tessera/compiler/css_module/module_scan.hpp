// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::css_module {

// 6 lowercase hex digits of FNV-1a 64 over the project-relative POSIX path.
[[nodiscard]] std::string module_hash(std::string_view project_relative_path);

// `<local>_<hash>`.
[[nodiscard]] std::string scoped_name(std::string_view local, std::string_view hash);

struct ModuleClass {
    std::string local;
    std::string scoped;
};

struct ModuleKeyframes {
    std::string local;
    std::string scoped;
};

struct ModuleScan {
    std::string hash;
    std::vector<ModuleClass> classes;       // sorted by `local`, unique
    std::vector<ModuleKeyframes> keyframes; // sorted by `local`, unique
    std::string rewritten;                  // the CSS text with scoped names
};

struct ModuleScanError {
    std::string code;    // a TESSERA_CSS_MODULE_* code
    std::string message; // one line, names the offending construct
    std::size_t offset = 0;
};

[[nodiscard]] std::expected<ModuleScan, ModuleScanError>
scan_module_css(std::string_view project_relative_path, std::string_view text);

} // namespace tessera::compiler::css_module
