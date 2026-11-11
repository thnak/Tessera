// SPDX-License-Identifier: MIT
//
// Scaled JPEG decode (issue #14): the IDCT reduces a large photo to 1/2, 1/4 or 1/8 so the full
// raster is never allocated. The result must have the size the target asks for, keep the source
// size, be byte-identical across calls, and stay close to a full decode reduced by the box filter.
// The JPEG is generated here with stb's encoder so no binary fixture is needed.

#include "jpeg_scaled.hpp"
#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/image/image_resize.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <stb_image_write.h>

namespace {

namespace image = tessera::rendering::image;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "jpeg_scaled: " << message << "\n";
    }
    return condition;
}

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// A smooth gradient with a diagonal edge: realistic for a photo and friendly to JPEG.
std::vector<std::uint8_t> make_jpeg(int width, int height) {
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
            rgb[i] = static_cast<std::uint8_t>(x * 255 / width);
            rgb[i + 1] = static_cast<std::uint8_t>(y * 255 / height);
            rgb[i + 2] = x * height > y * width ? 220 : 40;
        }
    }
    std::vector<std::uint8_t> jpeg;
    stbi_write_jpg_to_func(append, &jpeg, width, height, 3, rgb.data(), 90);
    return jpeg;
}

double mean_difference(const image::DecodedImage& a, const image::DecodedImage& b) {
    double total = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) {
        total += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    }
    return total / static_cast<double>(a.rgba.size());
}

bool denominators() {
    bool ok = expect(image::jpeg_scale_denominator(4000, 3000, 4000, 3000) == 1,
                     "no reduction needed keeps 1/1");
    ok = expect(image::jpeg_scale_denominator(4000, 3000, 2000, 1500) == 2, "exactly half") && ok;
    ok = expect(image::jpeg_scale_denominator(4000, 3000, 1999, 1500) == 2,
                "stays at 1/2 while the image still covers the target") &&
         ok;
    ok = expect(image::jpeg_scale_denominator(4000, 3000, 600, 450) == 4, "1/4 for a thumbnail") &&
         ok;
    ok = expect(image::jpeg_scale_denominator(4000, 3000, 64, 48) == 8, "1/8 is the limit") && ok;
    ok = expect(image::jpeg_scale_denominator(101, 51, 51, 26) == 2,
                "odd sizes round up, as libjpeg does") &&
         ok;
    return ok;
}

bool scaled_decode() {
    const std::vector<std::uint8_t> jpeg = make_jpeg(1024, 768);
    image::ImageLimits limits;
    limits.max_width = 4096;
    limits.max_height = 4096;
    const image::DecodeTarget target{128, 96};
    const auto reduced = image::decode_image(jpeg, limits, target);
    const auto again = image::decode_image(jpeg, limits, target);
    const auto original = image::decode_image(jpeg, limits, {});
    bool ok = expect(reduced && again && original, "all three decodes succeed");
    if (!ok) {
        return false;
    }
    const image::ImageSize wanted = image::fit_decode_size(1024, 768, target);
    ok = expect(reduced->width == wanted.width && reduced->height == wanted.height,
                "the raster has the size the target asks for") &&
         ok;
    ok = expect(reduced->source_width == 1024 && reduced->source_height == 768,
                "the source size is kept for layout") &&
         ok;
    ok = expect(reduced->rgba.size() == static_cast<std::size_t>(wanted.width) * wanted.height * 4,
                "tightly packed RGBA") &&
         ok;
    ok = expect(reduced->rgba == again->rgba, "the scaled decode is deterministic") && ok;
    ok = expect(original->width == 1024 && original->height == 768, "original stays full size") &&
         ok;

    // Reduce the full decode with the box filter and compare: the IDCT scale is a different
    // filter, so allow a small mean difference rather than equality.
    const image::DecodedImage reference =
        image::downscale_box(*original, wanted.width, wanted.height);
    ok = expect(mean_difference(*reduced, reference) < 4.0,
                "close to a full decode reduced by the box filter") &&
         ok;
    return ok;
}

bool falls_back() {
    const std::vector<std::uint8_t> jpeg = make_jpeg(256, 256);
    // A target that needs no reduction, and one barely smaller, must still decode.
    bool ok = expect(image::decode_image(jpeg, {}, {256, 256}).has_value(), "same-size target");
    const auto slight = image::decode_image(jpeg, {}, {200, 200});
    ok = expect(slight && slight->width == 200 && slight->height == 200,
                "a target between scale steps is finished by the box filter") &&
         ok;

    // Garbage after a valid header must not crash or hang; it is a value either way.
    std::vector<std::uint8_t> broken(jpeg.begin(), jpeg.begin() + 120);
    const auto result = image::decode_image(broken, {}, {32, 32});
    ok = expect(!result || result->width > 0, "a truncated JPEG does not crash") && ok;

    image::JpegRaster raster;
    ok = expect(!image::decode_jpeg_scaled({}, 2, raster), "empty input is declined") && ok;
    ok = expect(!image::decode_jpeg_scaled(jpeg, 3, raster), "an unsupported scale is declined") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = denominators();
    ok = scaled_decode() && ok;
    ok = falls_back() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "jpeg_scaled: ok\n";
    return 0;
}
