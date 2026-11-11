// SPDX-License-Identifier: MIT
//
// Decode at display size x DPR (issue #14): size buckets, covering-fit sizes that never enlarge,
// deterministic alpha-weighted box downscale, target-keyed decode cache entries, and the rule that
// limits are judged on the source size.

#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_loader.hpp"
#include "tessera/rendering/image/image_resize.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace image = tessera::rendering::image;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "image_resize: " << message << "\n";
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

bool buckets() {
    bool ok = expect(image::size_bucket(1) == 64 && image::size_bucket(64) == 64, "floor of 64");
    ok = expect(image::size_bucket(65) == 96 && image::size_bucket(96) == 96, "midpoint 96") && ok;
    ok = expect(image::size_bucket(97) == 128 && image::size_bucket(129) == 192, "next buckets") &&
         ok;
    ok = expect(image::size_bucket(600) == 768 && image::size_bucket(450) == 512, "phone sizes") &&
         ok;
    // 200 x 150 dp on a 3x screen is 600 x 450 px.
    const image::DecodeTarget t = image::decode_target_for(200, 150, 3);
    ok =
        expect(t.width == 768 && t.height == 512 && !t.original(), "dp x dpr, then bucketed") && ok;
    ok = expect(image::decode_target_for(0, 150, 3).original() &&
                    image::decode_target_for(200, 150, 0).original(),
                "nonsense sizes mean original") &&
         ok;
    return ok;
}

bool fit_sizes() {
    // A 4000 x 3000 photo into a 768 x 512 box: cover by width ratio 0.192 vs height 0.1707 ->
    // 0.192, so 768 x 576; never enlarged.
    const image::ImageSize photo = image::fit_decode_size(4000, 3000, {768, 512});
    bool ok = expect(photo.width == 768 && photo.height == 576, "covers the target box");
    const image::ImageSize small = image::fit_decode_size(100, 100, {768, 512});
    ok = expect(small.width == 100 && small.height == 100, "never enlarges") && ok;
    const image::ImageSize original = image::fit_decode_size(4000, 3000, {});
    ok = expect(original.width == 4000 && original.height == 3000, "original keeps the source") &&
         ok;
    const image::ImageSize wide = image::fit_decode_size(8000, 100, {64, 64});
    ok = expect(wide.width == 5120 && wide.height == 64, "the shorter side still covers") && ok;
    return ok;
}

bool downscale() {
    image::DecodedImage source;
    source.width = 4;
    source.height = 2;
    source.rgba = {255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255,
                   255, 0, 0, 255, 255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255};
    const image::DecodedImage out = image::downscale_box(source, 2, 1);
    bool ok =
        expect(out.width == 2 && out.height == 1 && out.source_width == 4 && out.source_height == 2,
               "size and source size");
    ok = expect(pixel(out, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255} &&
                    pixel(out, 1, 0) == std::array<std::uint8_t, 4>{0, 0, 255, 255},
                "flat blocks keep their color") &&
         ok;

    // Alpha weighting: an opaque white pixel next to a transparent black one averages to white at
    // half alpha, not to gray.
    image::DecodedImage edge;
    edge.width = 2;
    edge.height = 1;
    edge.rgba = {255, 255, 255, 255, 0, 0, 0, 0};
    const image::DecodedImage mixed = image::downscale_box(edge, 1, 1);
    ok = expect(pixel(mixed, 0, 0) == std::array<std::uint8_t, 4>{255, 255, 255, 128},
                "transparent pixels do not bleed color") &&
         ok;

    ok = expect(image::downscale_box(source, 2, 1).rgba == out.rgba, "deterministic") && ok;
    return ok;
}

bool decode_to_target() {
    const std::vector<std::uint8_t> bytes = read_fixture("checker.png");
    const auto full = image::decode_image(bytes);
    const auto half = image::decode_image(bytes, {}, {4, 4});
    if (!expect(full.has_value() && half.has_value(), "both decodes succeed")) {
        return false;
    }
    bool ok = expect(half->width == 4 && half->height == 4 && half->source_width == 8 &&
                         half->source_height == 8 && half->rgba.size() == 4 * 4 * 4,
                     "reduced to 4 x 4, source size recorded");
    ok = expect(full->source_width == 8 && full->width == 8, "an original decode is unchanged") &&
         ok;
    ok = expect(pixel(*half, 0, 0) == std::array<std::uint8_t, 4>{255, 0, 0, 255} &&
                    pixel(*half, 3, 3) == std::array<std::uint8_t, 4>{255, 255, 255, 128},
                "quadrants survive the reduction") &&
         ok;
    const auto bigger = image::decode_image(bytes, {}, {64, 64});
    ok = expect(bigger.has_value() && bigger->width == 8, "a larger target does not enlarge") && ok;

    // Limits are judged on the encoded size, not the reduced raster.
    image::ImageLimits tight;
    tight.max_width = 4;
    ok = expect(!image::decode_image(bytes, tight, {4, 4}).has_value(),
                "limits apply to the source size") &&
         ok;
    return ok;
}

bool cache_keys() {
    const std::vector<std::uint8_t> bytes = read_fixture("checker.png");
    image::ImageDecodeCache cache;

    const auto a = cache.acquire(bytes, {}, {4, 4});
    const auto b = cache.acquire(bytes, {}, {4, 4});
    bool ok = expect(a.has_value() && b.has_value() && a->get() == b->get(), "same bucket hits");
    ok = expect(cache.stats().decodes == 1 && cache.stats().hits == 1, "one decode, one hit") && ok;
    ok = expect(cache.stats().bytes == 4 * 4 * 4, "the cache holds the reduced raster") && ok;

    const auto c = cache.acquire(bytes, {}, {2, 2});
    ok = expect(c.has_value() && c.value()->width == 2 && cache.stats().decodes == 2,
                "a smaller bucket decodes again") &&
         ok;

    // A target that does not shrink the image decodes at source size and is shared with original.
    const auto big = cache.acquire(bytes, {}, {64, 64});
    const auto original = cache.acquire(bytes);
    ok = expect(big.has_value() && original.has_value() && big->get() == original->get() &&
                    cache.stats().decodes == 3,
                "non-shrinking targets share the source-size entry") &&
         ok;
    const auto big_again = cache.acquire(bytes, {}, {96, 96});
    ok = expect(big_again.has_value() && big_again->get() == original->get() &&
                    cache.stats().decodes == 3,
                "another non-shrinking target hits it too") &&
         ok;

    image::ImageLimits tight;
    tight.max_width = 4;
    ok = expect(!cache.acquire(bytes, tight, {4, 4}).has_value(),
                "a hit is still judged against the caller's limits on source size") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = buckets();
    ok = fit_sizes() && ok;
    ok = downscale() && ok;
    ok = decode_to_target() && ok;
    ok = cache_keys() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "image_resize: ok\n";
    return 0;
}
