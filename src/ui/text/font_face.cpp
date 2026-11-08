// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_face.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::text {
namespace detail {

struct FontFaceState {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::vector<std::uint8_t> owned_bytes;

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

} // namespace detail
namespace {

FontError error_from(FT_Error error, FontErrorCode fallback, std::string_view context) {
    FontErrorCode code = fallback;
    if (error == FT_Err_Cannot_Open_Resource) {
        code = FontErrorCode::file_not_found;
    } else if (error == FT_Err_Unknown_File_Format) {
        code = FontErrorCode::invalid_font;
    } else if (error == FT_Err_Invalid_Argument || error == FT_Err_Invalid_File_Format) {
        code = FontErrorCode::index_out_of_range;
    }
    std::string message(context);
    message += " (FreeType error ";
    message += std::to_string(static_cast<int>(error));
    message += ')';
    return FontError{code, std::move(message)};
}

bool make_scalable(const detail::FontFaceState& state) {
    if (!FT_IS_SCALABLE(state.face)) {
        return false;
    }
    // Text runs are indexed by Unicode codepoint; a face without a Unicode charmap cannot
    // answer has_glyph, so leave the default map in place and let the caller decide.
    FT_Select_Charmap(state.face, FT_ENCODING_UNICODE);
    return true;
}

} // namespace

FontFace::FontFace(std::shared_ptr<detail::FontFaceState> state)
    : state_(std::move(state)) {}

std::expected<FontFace, FontError> FontFace::load_from_file(std::string_view path,
                                                            std::uint32_t face_index) {
    if (path.empty()) {
        return std::unexpected(FontError{FontErrorCode::file_not_found, "font path is empty"});
    }

    auto state = std::make_shared<detail::FontFaceState>();
    const std::string path_string(path);
    FT_Error error = FT_Init_FreeType(&state->library);
    if (error != 0) {
        return std::unexpected(
            error_from(error, FontErrorCode::invalid_font, "initializing FreeType"));
    }
    error = FT_New_Face(state->library, path_string.c_str(), static_cast<FT_Long>(face_index),
                        &state->face);
    if (error != 0) {
        return std::unexpected(error_from(error, FontErrorCode::invalid_font, "opening font file"));
    }
    if (!make_scalable(*state)) {
        return std::unexpected(FontError{FontErrorCode::not_scalable, "font face is not scalable"});
    }
    return FontFace(std::move(state));
}

std::expected<FontFace, FontError> FontFace::load_from_memory(std::span<const std::uint8_t> data,
                                                              std::uint32_t face_index) {
    if (data.empty()) {
        return std::unexpected(FontError{FontErrorCode::invalid_font, "font data is empty"});
    }

    auto state = std::make_shared<detail::FontFaceState>();
    FT_Error error = FT_Init_FreeType(&state->library);
    if (error != 0) {
        return std::unexpected(
            error_from(error, FontErrorCode::invalid_font, "initializing FreeType"));
    }

    // FreeType keeps a pointer to the buffer for the lifetime of the face, so the bytes move
    // into the shared state instead of referencing caller memory.
    state->owned_bytes.assign(data.begin(), data.end());
    error = FT_New_Memory_Face(state->library, state->owned_bytes.data(),
                               static_cast<FT_Long>(state->owned_bytes.size()),
                               static_cast<FT_Long>(face_index), &state->face);
    if (error != 0) {
        return std::unexpected(error_from(error, FontErrorCode::invalid_font, "loading font data"));
    }
    if (!make_scalable(*state)) {
        return std::unexpected(FontError{FontErrorCode::not_scalable, "font face is not scalable"});
    }
    return FontFace(std::move(state));
}

bool FontFace::valid() const noexcept {
    return state_ != nullptr && state_->face != nullptr;
}

FontFaceMetrics FontFace::design_metrics() const noexcept {
    if (!valid()) {
        return {};
    }
    const FT_Face face = state_->face;
    return FontFaceMetrics{
        static_cast<std::uint16_t>(face->units_per_EM),
        static_cast<std::int16_t>(face->ascender),
        static_cast<std::int16_t>(face->descender),
        static_cast<std::int16_t>(face->height),
    };
}

std::uint32_t FontFace::face_count() const noexcept {
    if (!valid()) {
        return 0;
    }
    return static_cast<std::uint32_t>(state_->face->num_faces);
}

bool FontFace::has_glyph(char32_t codepoint) const noexcept {
    if (!valid()) {
        return false;
    }
    return FT_Get_Char_Index(state_->face, static_cast<FT_ULong>(codepoint)) != 0;
}

std::string FontFace::family_name() const {
    if (!valid() || state_->face->family_name == nullptr) {
        return {};
    }
    return state_->face->family_name;
}

} // namespace tessera::ui::text
