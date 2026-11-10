// SPDX-License-Identifier: MIT

// DPI -> render-scale quantization: the decided 1.0/1.25/1.5/2.0 ladder, nearest rounding with
// ties up, clamping at both ends, and rejection of invalid device scales.

#include "tessera/ui/scale/render_scale.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <iostream>
#include <limits>

namespace {

using tessera::ui::scale::device_scale_for_dpi;
using tessera::ui::scale::kReferenceDpi;
using tessera::ui::scale::kRenderScaleLadder;
using tessera::ui::scale::kRenderScaleStepCount;
using tessera::ui::scale::ladder_index;
using tessera::ui::scale::quantize_device_scale;
using tessera::ui::scale::quantize_render_scale;
using tessera::ui::scale::render_scale_for_dpi;
using tessera::ui::scale::RenderScaleError;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scale_quantize: " << message << "\n";
    }
    return condition;
}

// `near` is only used for the fractional 100 dpi row, where dpi/96 is not exactly representable.
bool near(float value, float expected, float tolerance = 1.0e-6f) {
    return value > expected - tolerance && value < expected + tolerance;
}

struct DpiRow {
    std::uint32_t dpi;
    float expected;
};

// 96/120/144/192 are the 100/125/150/200% desktop steps. The rest are in-between scales that a
// fractional desktop, a per-monitor DPI query, or a RandR re-query reports.
constexpr std::array<DpiRow, 19> kDpiTable{{
    {96, 1.0f},   // 100%
    {100, 1.0f},  // 1.0417 -> nearest is 1.0
    {108, 1.25f}, // 1.125 is an exact tie -> rounds up
    {110, 1.25f}, // 1.1458
    {112, 1.25f}, // 1.1667
    {116, 1.25f}, // 1.2083
    {120, 1.25f}, // 125%
    {126, 1.25f}, // 1.3125
    {128, 1.25f}, // 1.3333
    {132, 1.5f},  // 1.375 is an exact tie -> rounds up
    {138, 1.5f},  // 1.4375
    {144, 1.5f},  // 150%
    {160, 1.5f},  // 1.6667 -> 1.5 wins over the 2.0 step
    {168, 2.0f},  // 1.75 is an exact tie between 1.5 and 2.0 -> rounds up
    {180, 2.0f},  // 1.875
    {192, 2.0f},  // 200%
    {72, 1.0f},   // 0.75 clamps to the ladder floor
    {240, 2.0f},  // 2.5 clamps to the ladder ceiling
    {288, 2.0f},  // 3.0 clamps to the ladder ceiling
}};

struct TieRow {
    float device_scale;
    float expected;
};

// Exact ties and the values just below them: the tie rounds up, one step below it does not.
constexpr std::array<TieRow, 6> kTies{{
    {1.125f, 1.25f},
    {1.375f, 1.5f},
    {1.75f, 2.0f},
    {1.1249f, 1.0f},
    {1.3749f, 1.25f},
    {1.7499f, 1.5f},
}};

} // namespace

int main() {
    // The ladder is the decided four-value set, ascending, from 1.0 to 2.0.
    if (!expect(kRenderScaleStepCount == 4, "ladder has the four decided steps") ||
        !expect(kRenderScaleLadder.size() == kRenderScaleStepCount,
                "step count matches the array") ||
        !expect(kRenderScaleLadder.front() == 1.0f, "ladder starts at 1.0") ||
        !expect(kRenderScaleLadder.back() == 2.0f, "ladder ends at 2.0") ||
        !expect(kReferenceDpi == 96, "reference dpi is 96")) {
        return 1;
    }
    for (std::size_t i = 0; i < kRenderScaleStepCount; ++i) {
        if (!expect(ladder_index(kRenderScaleLadder[i]) == static_cast<std::int32_t>(i),
                    "ladder value maps to its own index") ||
            !expect(quantize_render_scale(kRenderScaleLadder[i]) == kRenderScaleLadder[i],
                    "a ladder value is its own quantization")) {
            return 2;
        }
        if (i != 0 && !expect(kRenderScaleLadder[i] > kRenderScaleLadder[i - 1],
                              "ladder is strictly ascending")) {
            return 3;
        }
    }

    // The DPI table, through both the dpi helper and the device-scale path.
    for (const DpiRow& row : kDpiTable) {
        const std::expected<float, RenderScaleError> scale = render_scale_for_dpi(row.dpi);
        if (!expect(scale.has_value(), "a positive dpi quantizes") ||
            !expect(near(*scale, row.expected), "dpi table step")) {
            std::cerr << "  dpi " << row.dpi << ": got " << (scale ? *scale : -1.0f) << " want "
                      << row.expected << "\n";
            return 4;
        }
        const std::expected<float, RenderScaleError> via_device =
            quantize_device_scale(static_cast<float>(row.dpi) / static_cast<float>(kReferenceDpi));
        if (!expect(via_device.has_value() && near(*via_device, row.expected),
                    "device scale quantizes to the same step")) {
            return 5;
        }
    }

    // Ties round up; a hair below a tie stays on the cheaper step.
    for (const TieRow& row : kTies) {
        if (!expect(quantize_render_scale(row.device_scale) == row.expected,
                    "tie rounds up, non-tie rounds to nearest")) {
            std::cerr << "  device scale " << row.device_scale << ": got "
                      << quantize_render_scale(row.device_scale) << " want " << row.expected
                      << "\n";
            return 6;
        }
    }

    // Clamping: below and above the ladder, including the 0/negative and non-finite inputs that
    // WindowMetrics must never contain but the paint path must still survive.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    if (!expect(quantize_render_scale(0.5f) == 1.0f, "0.5 clamps up to 1.0") ||
        !expect(quantize_render_scale(0.0f) == 1.0f, "0.0 clamps up to 1.0") ||
        !expect(quantize_render_scale(-2.0f) == 1.0f, "negative clamps up to 1.0") ||
        !expect(quantize_render_scale(3.0f) == 2.0f, "3.0 clamps down to 2.0") ||
        !expect(quantize_render_scale(2.5f) == 2.0f, "2.5 clamps down to 2.0") ||
        !expect(quantize_render_scale(nan) == 1.0f, "NaN quantizes to the ladder floor") ||
        !expect(quantize_render_scale(inf) == 1.0f, "infinity quantizes to the ladder floor") ||
        !expect(quantize_render_scale(-inf) == 1.0f, "-infinity quantizes to the ladder floor")) {
        return 7;
    }

    // Invalid device scales are reported, not clamped, through the checked path.
    const std::array<float, 5> kInvalid{nan, inf, -inf, 0.0f, -1.0f};
    for (const float device_scale : kInvalid) {
        const std::expected<float, RenderScaleError> quantized =
            quantize_device_scale(device_scale);
        if (!expect(!quantized.has_value(), "invalid device scale is rejected") ||
            !expect(quantized.error() == RenderScaleError::InvalidDeviceScale,
                    "invalid device scale reports InvalidDeviceScale")) {
            return 8;
        }
    }
    if (!expect(quantize_device_scale(1.5f) == 1.5f, "valid device scale is accepted")) {
        return 9;
    }

    // dpi 0 is not a display; every other dpi maps through dpi/96.
    const std::expected<float, RenderScaleError> zero_dpi = device_scale_for_dpi(0);
    if (!expect(!zero_dpi.has_value(), "dpi 0 is rejected") ||
        !expect(zero_dpi.error() == RenderScaleError::InvalidDpi, "dpi 0 reports InvalidDpi") ||
        !expect(!render_scale_for_dpi(0).has_value(), "dpi 0 has no render scale") ||
        !expect(device_scale_for_dpi(96) == 1.0f, "96 dpi is device scale 1.0") ||
        !expect(device_scale_for_dpi(144) == 1.5f, "144 dpi is device scale 1.5") ||
        !expect(device_scale_for_dpi(192) == 2.0f, "192 dpi is device scale 2.0")) {
        return 10;
    }

    // Values that are not on the ladder are not ladder values.
    if (!expect(ladder_index(1.75f) == -1, "1.75 is not a ladder step") ||
        !expect(ladder_index(0.9f) == -1, "0.9 is not a ladder step") ||
        !expect(ladder_index(2.0001f) == -1, "2.0001 is not a ladder step") ||
        !expect(ladder_index(nan) == -1, "NaN is not a ladder step")) {
        return 11;
    }

    // Sweep: quantization is monotonic, always lands on the ladder, and never exceeds the bounds.
    float previous = 0.0f;
    for (float device_scale = 0.25f; device_scale <= 3.5f; device_scale += 0.01f) {
        const float scale = quantize_render_scale(device_scale);
        if (!expect(scale >= previous, "quantization is monotonic") ||
            !expect(ladder_index(scale) >= 0, "quantization lands on the ladder") ||
            !expect(scale >= 1.0f && scale <= 2.0f, "quantization stays in bounds")) {
            return 12;
        }
        previous = scale;
    }

    std::cout << "scale_quantize: ok\n";
    return 0;
}
