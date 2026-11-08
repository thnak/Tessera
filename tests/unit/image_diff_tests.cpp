// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/image_diff.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>

namespace {

using tessera::rendering::golden::compare_rgba;
using tessera::rendering::golden::within_tolerance;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "image_diff: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    std::array<std::uint8_t, 16> base{};
    for (std::size_t i = 0; i < 4; ++i) {
        base[i * 4 + 0] = static_cast<std::uint8_t>(10 + i);
        base[i * 4 + 1] = static_cast<std::uint8_t>(20 + i);
        base[i * 4 + 2] = static_cast<std::uint8_t>(30 + i);
        base[i * 4 + 3] = 255;
    }
    const std::span<const std::uint8_t> base_span(base.data(), base.size());

    const auto same = compare_rgba({2, 2}, base_span, {2, 2}, base_span, 0);
    if (!expect(same.pixels == 4 && same.mismatched == 0, "identical pixels") ||
        !expect(same.max_channel_delta == 0 && same.mean_channel_delta() == 0.0, "zero deltas") ||
        !expect(within_tolerance(same, 0.0), "identical within tolerance")) {
        return 1;
    }

    std::array<std::uint8_t, 16> changed = base;
    changed[1] = static_cast<std::uint8_t>(changed[1] + 3);
    const std::span<const std::uint8_t> changed_span(changed.data(), changed.size());
    const auto diff = compare_rgba({2, 2}, changed_span, {2, 2}, base_span, 0);
    if (!expect(diff.mismatched == 1 && diff.first_mismatch == 0, "one mismatched pixel") ||
        !expect(diff.max_channel_delta == 3, "max delta") ||
        !expect(diff.mismatch_ratio() == 0.25, "mismatch ratio") ||
        !expect(diff.mean_channel_delta() == 3.0 / 16.0, "mean delta") ||
        !expect(within_tolerance(diff, 0.25), "ratio budget") ||
        !expect(!within_tolerance(diff, 0.24), "ratio over budget")) {
        return 2;
    }

    const auto tolerant = compare_rgba({2, 2}, changed_span, {2, 2}, base_span, 3);
    if (!expect(tolerant.mismatched == 0, "per-channel tolerance") ||
        !expect(tolerant.max_channel_delta == 3, "tolerance keeps max delta") ||
        !expect(within_tolerance(tolerant, 0.0), "tolerant image passes")) {
        return 3;
    }

    const auto wrong_size = compare_rgba({2, 2}, changed_span, {3, 2}, base_span, 0);
    if (!expect(wrong_size.size_mismatch, "size mismatch flag") ||
        !expect(!within_tolerance(wrong_size, 1.0), "size mismatch fails") ||
        !expect(wrong_size.mismatch_ratio() == 1.0, "size mismatch ratio")) {
        return 4;
    }

    const auto short_buffer =
        compare_rgba({2, 2}, changed_span, {2, 2},
                     std::span<const std::uint8_t>(base.data(), base.size() - 1), 0);
    if (!expect(short_buffer.size_mismatch, "short buffer is rejected")) {
        return 5;
    }

    std::cout << "image_diff: ok\n";
    return 0;
}
