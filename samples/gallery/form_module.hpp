// SPDX-License-Identifier: MIT

// The gallery's CSS module (issue #463): `form.module.css` is the one stylesheet of FormPage.tsx.
// The page's class names are scoped at compile time by the same scan the compiler uses, so the
// sheet the gallery loads must be that scan's rewritten text, filed under the same
// project-relative path the generated page was lowered with. Anything else paints `.page` while
// the tree carries `page_<hash>`.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace gallery {

// The project-relative path of the module file inside the generated project. The codegen root is
// the folder that holds FormPage.tsx, so this is the bare file name; the scoped names hash it.
inline constexpr std::string_view kFormModulePath = "form.module.css";

struct FormModule {
    std::string css;                                         // the scan's rewritten text
    std::map<std::string, std::string, std::less<>> classes; // authored local name -> scoped name
};

// Scans `text`; prints the scan's own diagnostic and returns nullopt when it is rejected.
[[nodiscard]] std::optional<FormModule> load_form_module(std::string_view text);

// The scoped name of an authored class, or `local` unchanged when the module does not declare it
// (so `--click` keeps working for a plain global class).
[[nodiscard]] std::string scoped_class(const FormModule& module, std::string_view local);

// `--click C[:N]` names an authored class; the tree carries the scoped one. Keeps the `:N` suffix.
[[nodiscard]] std::string scope_click_spec(const FormModule& module, std::string_view spec);

// Writes `form.scoped.css` and `form.classes.json` (local -> scoped) into `dir`, so the Chrome
// parity script renders the browser twin with exactly the names and rules the engine used. False
// when a file cannot be written.
[[nodiscard]] bool write_parity_twin(const FormModule& module, const std::string& dir);

} // namespace gallery
