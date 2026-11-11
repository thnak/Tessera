// SPDX-License-Identifier: MIT
//
// WebP decode (issue #14): detection by signature, straight-alpha RGBA output, scaled decode inside
// libwebp, the first frame of an animated file, limits judged on the encoded size, and rejection of
// corrupt input as a value. Fixtures come from tests/fixtures/assets/make_webp_fixtures.py.

#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/image/image_resize.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace image = tessera::rendering::image;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "webp_decode: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> read_fixture(const char* name) {
    std::ifstream file(std::string(TESSERA_IMAGE_FIXTURE_DIR) + "/" + name, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
}

std::array<std::uint8_t, 4> pixel(const image::DecodedImage& img, std::uint32_t x,
                                  std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * img.width + x) * 4;
    return {img.rgba[i], img.rgba[i + 1], img.rgba[i + 2], img.rgba[i + 3]};
}

bool near(int a, int b, int tolerance) {
    return a - b <= tolerance && b - a <= tolerance;
}

double mean_difference(const image::DecodedImage& a, const image::DecodedImage& b) {
    double total = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        total += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    }
    return total / static_cast<double>(a.rgba.size());
}

bool still_images() {
    const auto photo_bytes = read_fixture("photo.webp");
    bool ok = expect(!photo_bytes.empty(), "photo.webp fixture is present");
    if (!ok) {
        return false;
    }
    ok = expect(image::detect_format(photo_bytes) == image::ImageFormat::webp,
                "WebP is detected by signature") &&
         ok;
    ok = expect(image::format_name(image::ImageFormat::webp) == "webp", "format name") && ok;
    const auto full = image::decode_image(photo_bytes);
    ok = expect(full && full->width == 512 && full->height == 384 &&
                    full->format == image::ImageFormat::webp,
                "a still WebP decodes at source size") &&
         ok;
    if (!full) {
        return false;
    }
    ok = expect(full->rgba.size() == 512U * 384U * 4U && pixel(*full, 10, 10)[3] == 255,
                "opaque RGBA output") &&
         ok;
    // The gradient: red grows with x, green with y (lossy, so a tolerance).
    ok = expect(near(pixel(*full, 400, 10)[0], 400 * 255 / 512, 12) &&
                    near(pixel(*full, 10, 300)[1], 300 * 255 / 384, 12),
                "pixel values follow the source gradient") &&
         ok;

    const image::DecodeTarget target{128, 96};
    const auto reduced = image::decode_image(photo_bytes, {}, target);
    const auto again = image::decode_image(photo_bytes, {}, target);
    ok = expect(reduced && again, "scaled decode succeeds") && ok;
    if (reduced && again) {
        const image::ImageSize wanted = image::fit_decode_size(512, 384, target);
        ok = expect(reduced->width == wanted.width && reduced->height == wanted.height,
                    "the raster has the size the target asks for") &&
             ok;
        ok = expect(reduced->source_width == 512 && reduced->source_height == 384,
                    "the source size is kept for layout") &&
             ok;
        ok = expect(reduced->rgba == again->rgba, "the scaled decode is deterministic") && ok;
        const image::DecodedImage reference =
            image::downscale_box(*full, wanted.width, wanted.height);
        ok = expect(mean_difference(reference, *reduced) < 6.0,
                    "close to a full decode reduced by the box filter") &&
             ok;
    }
    const auto big_target = image::decode_image(photo_bytes, {}, {4096, 4096});
    ok = expect(big_target && big_target->width == 512,
                "a target above the source never enlarges") &&
         ok;
    return ok;
}

bool alpha_image() {
    const auto bytes = read_fixture("alpha.webp");
    const auto decoded = image::decode_image(bytes);
    bool ok =
        expect(decoded && decoded->width == 64 && decoded->height == 48, "alpha WebP decodes");
    if (!decoded) {
        return false;
    }
    ok = expect(pixel(*decoded, 8, 8)[3] == 0, "the transparent half is transparent") && ok;
    const auto right = pixel(*decoded, 50, 8);
    ok = expect(right[3] == 255 && right[1] > 150 && right[0] < 50,
                "the opaque half keeps its color") &&
         ok;
    const auto reduced = image::decode_image(bytes, {}, {32, 24});
    ok = expect(reduced && reduced->width == 32, "alpha scaled decode") && ok;
    return ok;
}

bool animated_image() {
    const auto bytes = read_fixture("anim.webp");
    const auto decoded = image::decode_image(bytes);
    bool ok = expect(decoded && decoded->width == 64 && decoded->height == 48,
                     "an animated WebP decodes");
    if (!decoded) {
        return false;
    }
    const auto first = pixel(*decoded, 30, 20);
    ok = expect(first[0] > 150 && first[2] < 60 && first[3] == 255,
                "only the first frame (red) is used") &&
         ok;
    const auto reduced = image::decode_image(bytes, {}, {32, 24});
    ok = expect(reduced && reduced->width == 32 && reduced->height == 24 &&
                    reduced->source_width == 64,
                "an animated WebP reduces to the target") &&
         ok;
    return ok;
}

bool limits_and_errors() {
    const auto bytes = read_fixture("photo.webp");
    image::ImageLimits small;
    small.max_width = 256;
    const auto limited = image::decode_image(bytes, small);
    bool ok = expect(!limited && limited.error() == image::ImageError::limit_exceeded,
                     "limits are judged on the encoded size");
    const auto limited_reduced = image::decode_image(bytes, small, {64, 64});
    ok = expect(!limited_reduced && limited_reduced.error() == image::ImageError::limit_exceeded,
                "a reduced decode obeys the source limits") &&
         ok;
    const std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
    ok = expect(!image::decode_image(truncated).has_value(),
                "a truncated WebP is a value, not a crash") &&
         ok;
    const std::vector<std::uint8_t> header_only(bytes.begin(), bytes.begin() + 12);
    const auto bare = image::decode_image(header_only);
    ok = expect(!bare && bare.error() == image::ImageError::malformed,
                "a bare RIFF/WEBP header is malformed") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = still_images();
    ok = alpha_image() && ok;
    ok = animated_image() && ok;
    ok = limits_and_errors() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "webp_decode: ok\n";
    return 0;
}
