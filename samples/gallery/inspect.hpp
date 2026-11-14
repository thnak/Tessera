// SPDX-License-Identifier: MIT

// Inspection matrix for the gallery app (developer visibility, not a test oracle).
//
// `samples/gallery` has two output paths: `--capture out.png` renders on ANY Vulkan device, and
// `--golden base.png` compares against a committed lavapipe baseline. The golden path is gated to
// lavapipe on purpose (harness::capture_capability): only one deterministic rasterizer can back a
// byte-comparable baseline. The consequence is that the 23 committed baselines under
// tests/golden/gallery/ SKIP on every machine with a hardware GPU, so a developer never sees what
// the app looks like while changing it.
//
// `--inspect DIR` closes that gap without weakening the gate. It drives the same case matrix as
// the CTest goldens through the ungated capture path and writes one PNG per case, plus an
// index.html contact sheet. The images are for looking at; the CI goldens stay the authority.

#pragma once

#include "tessera/app/window.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace gallery_inspect {

// One case: the gallery switches a developer would otherwise pass by hand. Mirrors the CTest cases
// in samples/gallery/CMakeLists.txt so a reviewed image maps to a named golden. Keep both lists in
// sync when a case is added.
struct Case {
    std::string name;
    std::string theme = "instrument-light";
    std::string density = "comfortable";
    std::string state = "none";
    int text_scale = 100;
    bool rtl = false;
    std::string page = "buttons";
    std::string device = "none";
};

// The full matrix: theme x state, plus density, text scale and RTL, plus one screen per scripted
// safe-area device in both themes.
[[nodiscard]] std::vector<Case> case_matrix();

// Renders one case. Returns nullopt when the machine has no Vulkan device. Supplied by the caller
// (main.cpp), which owns the scene, style sheets and theme wiring.
using RenderCase = std::function<std::optional<tessera::app::CapturedFrame>(const Case&)>;

// Drives `render` over case_matrix(), writes <out_dir>/<name>.png for each and an index.html
// contact sheet. Returns a process exit code: 0 on success (including a documented device skip),
// 4 when a file cannot be written.
[[nodiscard]] int run(const std::string& out_dir, const RenderCase& render);

} // namespace gallery_inspect
