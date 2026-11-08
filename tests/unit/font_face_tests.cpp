// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_face.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <utility>
#include <vector>

namespace {

using tessera::ui::text::FontErrorCode;
using tessera::ui::text::FontFace;
using tessera::ui::text::FontFaceMetrics;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "font_face: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "font_face: missing font path" << "\n";
        return 1;
    }
    const char* font_path = argv[1];

    const auto loaded = FontFace::load_from_file(font_path);
    if (!loaded) {
        std::cerr << "font_face: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }
    const FontFace& face = *loaded;

    if (!expect(face.valid(), "loaded face is valid") ||
        !expect(static_cast<bool>(face), "explicit bool conversion") ||
        !expect(face.face_count() >= 1, "face count") ||
        !expect(face.family_name() == "Noto Sans", "family name")) {
        return 2;
    }

    const FontFaceMetrics metrics = face.design_metrics();
    if (!expect(metrics.units_per_em > 0, "units per em") ||
        !expect(metrics.ascender > 0, "positive ascender") ||
        !expect(metrics.descender < 0, "negative descender") ||
        !expect(metrics.height >= metrics.ascender - metrics.descender, "line height")) {
        return 3;
    }

    if (!expect(face.has_glyph(U'A'), "glyph for A") ||
        !expect(face.has_glyph(U'\u20AC'), "glyph for euro sign") ||
        !expect(!face.has_glyph(U'\U0010FFFF'), "no glyph for U+10FFFF")) {
        return 4;
    }

    const std::vector<std::uint8_t> bytes = read_file(font_path);
    const auto from_memory = FontFace::load_from_memory(bytes);
    if (!from_memory) {
        std::cerr << "font_face: cannot load fixture bytes: " << from_memory.error().message
                  << "\n";
        return 5;
    }
    if (!expect(from_memory->design_metrics().units_per_em == metrics.units_per_em,
                "memory face units per em") ||
        !expect(from_memory->has_glyph(U'A'), "memory face glyph") ||
        !expect(from_memory->family_name() == face.family_name(), "memory face family")) {
        return 6;
    }

    FontFace copied = face;
    if (!expect(copied.valid(), "copied handle is valid") ||
        !expect(copied.design_metrics().ascender == metrics.ascender, "copied handle metrics")) {
        return 7;
    }

    FontFace moved = std::move(copied);
    if (!expect(moved.valid(), "moved handle is valid") ||
        !expect(!copied.valid(), "moved-from handle is invalid")) {
        return 8;
    }

    const auto missing = FontFace::load_from_file("tessera-no-such-font.ttf");
    if (!expect(!missing, "missing file fails") ||
        !expect(missing.error().code == FontErrorCode::file_not_found, "missing file error code") ||
        !expect(!missing.error().message.empty(), "missing file message")) {
        return 9;
    }

    const std::vector<std::uint8_t> garbage(64, 0);
    const auto invalid = FontFace::load_from_memory(garbage);
    if (!expect(!invalid, "garbage fails") ||
        !expect(invalid.error().code == FontErrorCode::invalid_font, "garbage error code")) {
        return 10;
    }

    const auto empty = FontFace::load_from_memory({});
    if (!expect(!empty, "empty data fails") ||
        !expect(empty.error().code == FontErrorCode::invalid_font, "empty data error code")) {
        return 11;
    }

    const FontFace default_face;
    if (!expect(!default_face.valid(), "default face is invalid") ||
        !expect(!default_face.has_glyph(U'A'), "default face has no glyphs") ||
        !expect(default_face.face_count() == 0, "default face count") ||
        !expect(default_face.design_metrics().units_per_em == 0, "default face metrics")) {
        return 12;
    }

    std::cout << "font_face: ok\n";
    return 0;
}
