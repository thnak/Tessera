// SPDX-License-Identifier: MIT
//
// Raster image decode contract: PNG/JPEG support, exact RGBA8 output for the PNG fixture, bounded
// input/dimension/pixel caps, rejection of corrupt/unsupported/16-bit input as values, and
// deterministic repeated decoding. Fixtures are hand-authored MIT assets under tests/fixtures.

#include "tessera/rendering/image/image_loader.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace image = tessera::rendering::image;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "image_loader: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] std::vector<std::uint8_t> read_fixture(const char* name) {
    const std::string path = std::string(TESSERA_IMAGE_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "image_loader: cannot open fixture: " << path << "\n";
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::array<std::uint8_t, 4> pixel(const image::DecodedImage& decoded, std::uint32_t x,
                                                std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * decoded.width + x) * 4;
    return {decoded.rgba[index], decoded.rgba[index + 1], decoded.rgba[index + 2],
            decoded.rgba[index + 3]};
}

bool decode_png_exact() {
    const std::vector<std::uint8_t> bytes = read_fixture("checker.png");
    const auto decoded = image::decode_image(bytes);
    if (!expect(decoded.has_value(), "the PNG fixture decodes")) {
        return false;
    }
    bool ok = expect(decoded->width == 8 && decoded->height == 8, "PNG dimensions are 8x8");
    ok = expect(decoded->format == image::ImageFormat::png, "PNG format is detected") && ok;
    ok = expect(decoded->content_hash == 0, "direct decode leaves the cache hash unset") && ok;
    ok = expect(pixel(*decoded, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255},
                "top-left quadrant is opaque red") &&
         ok;
    ok = expect(pixel(*decoded, 4, 0) == std::array<std::uint8_t, 4>{0, 255, 0, 255},
                "top-right quadrant is opaque green") &&
         ok;
    ok = expect(pixel(*decoded, 0, 4) == std::array<std::uint8_t, 4>{0, 0, 255, 255},
                "bottom-left quadrant is opaque blue") &&
         ok;
    ok = expect(pixel(*decoded, 4, 4) == std::array<std::uint8_t, 4>{255, 255, 255, 128},
                "bottom-right quadrant keeps its authored alpha") &&
         ok;

    const auto repeat = image::decode_image(bytes);
    ok = expect(repeat.has_value() && repeat->rgba == decoded->rgba,
                "repeated PNG decoding is byte-identical") &&
         ok;
    return ok;
}

bool decode_jpeg() {
    const std::vector<std::uint8_t> bytes = read_fixture("stripe.jpg");
    const auto decoded = image::decode_image(bytes);
    if (!expect(decoded.has_value(), "the JPEG fixture decodes")) {
        return false;
    }
    bool ok = expect(decoded->width == 8 && decoded->height == 8, "JPEG dimensions are 8x8");
    ok = expect(decoded->format == image::ImageFormat::jpeg, "JPEG format is detected") && ok;
    ok = expect(decoded->rgba.size() == 8U * 8U * 4U, "JPEG decodes to RGBA8") && ok;
    // JPEG is lossy: a uniform orange block decodes near its authored colour.
    const auto sample = pixel(*decoded, 3, 3);
    ok = expect(sample[0] > 180 && sample[1] > 80 && sample[2] > 30 && sample[3] == 255,
                "a uniform JPEG block decodes near the authored orange") &&
         ok;
    const auto repeat = image::decode_image(bytes);
    ok = expect(repeat.has_value() && repeat->rgba == decoded->rgba,
                "repeated JPEG decoding is byte-identical") &&
         ok;
    return ok;
}

bool rejects_with_values() {
    const std::vector<std::uint8_t> png = read_fixture("checker.png");
    const std::vector<std::uint8_t> jpeg = read_fixture("stripe.jpg");
    bool ok = expect(image::detect_format(png) == image::ImageFormat::png, "PNG signature");
    ok = expect(image::detect_format(jpeg) == image::ImageFormat::jpeg, "JPEG signature") && ok;

    ok = expect(image::decode_image({}).error() == image::ImageError::invalid_argument,
                "empty input is invalid_argument") &&
         ok;
    image::ImageLimits zero = {};
    zero.max_input_bytes = 0;
    ok = expect(image::decode_image(png, zero).error() == image::ImageError::invalid_argument,
                "zero limits are invalid_argument") &&
         ok;

    image::ImageLimits one_byte = {};
    one_byte.max_input_bytes = 1;
    ok = expect(image::decode_image(png, one_byte).error() == image::ImageError::input_too_large,
                "the input byte cap is enforced") &&
         ok;

    const std::vector<std::uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a'};
    ok = expect(image::decode_image(gif).error() == image::ImageError::unsupported_format,
                "non-PNG/JPEG input is unsupported_format") &&
         ok;

    const std::vector<std::uint8_t> truncated(png.begin(), png.begin() + 20);
    ok = expect(image::decode_image(truncated).error() == image::ImageError::malformed,
                "a truncated PNG is malformed") &&
         ok;

    image::ImageLimits narrow = {};
    narrow.max_width = 4;
    ok = expect(image::decode_image(png, narrow).error() == image::ImageError::limit_exceeded,
                "the width cap is enforced") &&
         ok;
    image::ImageLimits few_pixels = {};
    few_pixels.max_pixels = 10;
    ok = expect(image::decode_image(png, few_pixels).error() == image::ImageError::limit_exceeded,
                "the pixel budget is enforced") &&
         ok;

    const std::vector<std::uint8_t> sixteen = read_fixture("checker16.png");
    if (!sixteen.empty()) {
        ok = expect(image::decode_image(sixteen).error() == image::ImageError::unsupported_format,
                    "16-bit PNG is rejected as unsupported in this slice") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = decode_png_exact() && ok;
    ok = decode_jpeg() && ok;
    ok = rejects_with_values() && ok;
    if (!ok) {
        std::cerr << "image_loader: FAILED\n";
        return 1;
    }
    std::cout << "image_loader: ok\n";
    return 0;
}
