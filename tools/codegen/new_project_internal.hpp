// SPDX-License-Identifier: MIT

#pragma once

#include "new_project.hpp"

#include <string>
#include <string_view>

// The template bodies, split across two translation units so each stays reviewable: the project and
// build files (tessera.json, CMake, README) in `new_project_templates.cpp`, the UI and C++ sources
// the user actually edits (`ui/App.tsx`, `src/App.hpp`, `src/main.cpp`, ...) in
// `new_project_app_source.cpp`. Both are pure text: no filesystem, no clock, no environment.
namespace tessera::generator::internal {

// --- the app sources (new_project_app_source.cpp) -----------------------------------------------

// `ui/App.tsx` for a template. `counter` picks the stateful one; every other name is the blank one.
[[nodiscard]] std::string app_tsx(std::string_view template_name);

// `ui/app.css`: the stylesheet `ui.styles` names.
[[nodiscard]] std::string app_css(std::string_view app_name);

// `src/App.hpp` / `src/App.cpp`: the ADR-040 developer class.
[[nodiscard]] std::string app_header(const Options& options);
[[nodiscard]] std::string app_source();

// `src/main.cpp`: the app-window entry point (the samples/click-demo shape).
[[nodiscard]] std::string main_source(const Options& options);

} // namespace tessera::generator::internal
