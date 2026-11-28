// SPDX-License-Identifier: MIT

#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <vector>

namespace tessera::ui::text::detail {

// Internal FreeType state shared by font loading, rasterization, and metrics. Never included by
// public headers.
struct FontFaceState {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::vector<std::uint8_t> owned_bytes;
    // The collection index the face was opened with, kept so clone_independent() reopens the same
    // face.
    std::uint32_t face_index = 0;

    FontFaceState() = default;
    FontFaceState(const FontFaceState&) = delete;
    FontFaceState& operator=(const FontFaceState&) = delete;

    ~FontFaceState() {
        if (face != nullptr) {
            FT_Done_Face(face);
        }
        if (library != nullptr) {
            FT_Done_FreeType(library);
        }
    }
};

} // namespace tessera::ui::text::detail
