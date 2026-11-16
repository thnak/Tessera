// SPDX-License-Identifier: MIT

#pragma once

#include "preview_text.hpp"
#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Shared fixtures for the preview-frame tests (issue #167's frame path and issue #387's text half).
//
// Two translation units share this header so each stays inside the source-size budget:
// `scene_frame_tests.cpp` covers the frame (prepare, counters, determinism, the report, the
// tolerance policy) and `scene_frame_text_tests.cpp` covers text with a font. The failure counter
// is one object in this header, so a case in either file is reported once by that file's `main()`.
namespace scene_frame_test {

namespace cli = tessera::compiler::codegen::cli;
namespace null = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;

inline int& failures() {
    static int count = 0;
    return count;
}

inline void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_frame: " << message << "\n";
        ++failures();
    }
}

// The shipped face, as bytes. The tests read the same file the page fetches
// (`designer/web/public/`) rather than a fixture, so a change to the shipped font is what the gate
// sees. The path arrives as a compile definition because ctest may run from any directory, and an
// absolute build-time path is the only one that works from all of them.
inline std::vector<std::uint8_t> shipped_font() {
#ifdef TESSERA_PREVIEW_FONT_PATH
    constexpr std::string_view kPath = TESSERA_PREVIEW_FONT_PATH;
#else
    constexpr std::string_view kPath = "designer/web/public/fonts/IBMPlexSans-Latin.ttf";
#endif
    std::ifstream in(std::string(kPath), std::ios::binary);
    if (!in) {
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// One `Text` and two `Button`s: the smallest source that exercises all three payload kinds the
// preview paints, and the one both frame test files use.
inline constexpr std::string_view kColumn = R"(export function Column(): Node {
  return (
    <View>
      <Text>Preview</Text>
      <Button>Add</Button>
      <Button>Reset</Button>
    </View>
  );
}
)";

// A viewport and salt the ids are hashed with; the same values every preview consumer uses.
inline cli::ScenePreviewOptions options_for(std::string_view file) {
    cli::ScenePreviewOptions options;
    options.file = file;
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    return options;
}

// The text cases of `scene_frame_text_tests.cpp`, so its `main()` is one line.
void run_text_cases();

} // namespace scene_frame_test
