// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace tessera::ui::text {

enum class FontErrorCode : std::uint8_t {
    file_not_found,
    invalid_font,
    not_scalable,
    index_out_of_range,
};

struct FontError {
    FontErrorCode code = FontErrorCode::invalid_font;
    std::string message;
};

// Design-unit metrics from the font file. They are independent of the requested pixel size;
// FreeTypeFontMetricsProvider scales them for layout.
struct FontFaceMetrics {
    std::uint16_t units_per_em = 0;
    std::int16_t ascender = 0;
    std::int16_t descender = 0;
    std::int16_t height = 0;
};

namespace detail {
struct FontFaceState;
}

// A loaded, immutable font face. Copies share the underlying FreeType state; a face is safe to
// hand to several consumers but is not safe to use from several threads at once.
class FontFace {
public:
    [[nodiscard]] static std::expected<FontFace, FontError>
    load_from_file(std::string_view path, std::uint32_t face_index = 0);

    [[nodiscard]] static std::expected<FontFace, FontError>
    load_from_memory(std::span<const std::uint8_t> data, std::uint32_t face_index = 0);

    FontFace() = default;
    FontFace(const FontFace&) = default;
    FontFace(FontFace&&) noexcept = default;
    FontFace& operator=(const FontFace&) = default;
    FontFace& operator=(FontFace&&) noexcept = default;
    ~FontFace() = default;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    // Metrics are in font design units; callers scale by pixel_size / units_per_em.
    [[nodiscard]] FontFaceMetrics design_metrics() const noexcept;
    [[nodiscard]] std::uint32_t face_count() const noexcept;
    [[nodiscard]] bool has_glyph(char32_t codepoint) const noexcept;
    [[nodiscard]] std::string family_name() const;

private:
    explicit FontFace(std::shared_ptr<detail::FontFaceState> state);

    std::shared_ptr<detail::FontFaceState> state_;
};

} // namespace tessera::ui::text
