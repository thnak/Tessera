// SPDX-License-Identifier: MIT
//
// The gradient record math (gradient_math.hpp) and the per-frame table (gradient_table.hpp), which
// the fragment shader and the CPU reference share (docs/design/gpu/gradients.md, ADR-076):
//  - the shader-shaped ramp equals the banded path's `sample_gradient_stops` over a dense sweep, so
//    moving from bands to the shader changes no stop semantics (duplicate offsets, held ends, one
//    stop, the 32-stop repeating expansion);
//  - the position of every kind at known points: the CSS magic corners of a linear gradient, the
//    radius of a radial one, the four o'clock directions of a conic one with and without `from`;
//  - packing refuses what cannot be drawn, and the table hands out sequential records, advances
//    `first_stop`, stops at either capacity and resets.

#include "tessera/rendering/gpu/gradient_table.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

namespace gradient = tessera::rendering::gpu::gradient;
namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::GradientTable;
using tessera::ui::LinearGradientStop;

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "gradient_math: " << message << "\n";
    }
}

bool near(float a, float b, float tolerance = 1.0e-5f) {
    return std::abs(a - b) <= tolerance;
}

const rhi::Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
const rhi::Color kGreen{0.0f, 1.0f, 0.0f, 0.5f};
const rhi::Color kBlue{0.0f, 0.0f, 1.0f, 0.25f};

// Both representations of one ramp: the banded path's stop array and the record's flat arrays.
struct Ramp {
    std::vector<LinearGradientStop> stops;
    std::vector<float> colors;
    std::vector<float> offsets;

    explicit Ramp(std::vector<LinearGradientStop> source)
        : stops(std::move(source)) {
        for (const LinearGradientStop& stop : stops) {
            colors.insert(colors.end(), {stop.color.r, stop.color.g, stop.color.b, stop.color.a});
            offsets.push_back(stop.offset);
        }
    }
};

void ramp_matches_the_banded_sampler() {
    std::vector<LinearGradientStop> saw;
    for (int index = 0; index < 32; ++index) {
        saw.push_back({static_cast<float>(index) / 31.0f, index % 2 == 0 ? kRed : kBlue});
    }
    const std::vector<Ramp> ramps{
        Ramp({{0.0f, kRed}, {1.0f, kBlue}}),
        Ramp({{0.0f, kRed}, {0.3f, kGreen}, {1.0f, kBlue}}),
        Ramp({{0.0f, kRed}, {0.5f, kRed}, {0.5f, kBlue}, {1.0f, kBlue}}), // a hard edge
        Ramp({{0.25f, kRed}, {0.75f, kBlue}}),                            // ends held
        Ramp({{0.5f, kGreen}}),                                           // one stop
        Ramp({{0.0f, kRed}, {0.0f, kGreen}, {0.0f, kBlue}}),              // all at the start
        Ramp({{1.0f, kRed}, {1.0f, kBlue}}),                              // all at the end
        Ramp(saw),
    };
    int compared = 0;
    float worst = 0.0f;
    for (const Ramp& ramp : ramps) {
        for (int step = -250; step <= 1250; ++step) {
            const float t = static_cast<float>(step) / 1000.0f;
            const rhi::Color expected =
                tessera::ui::paint::sample_gradient_stops(ramp.stops.data(), ramp.stops.size(), t);
            const rhi::Color actual =
                gradient::ramp(ramp.colors.data(), ramp.offsets.data(),
                               static_cast<std::uint32_t>(ramp.stops.size()), t);
            worst =
                std::max({worst, std::abs(actual.r - expected.r), std::abs(actual.g - expected.g),
                          std::abs(actual.b - expected.b), std::abs(actual.a - expected.a)});
            ++compared;
        }
        // The exact stop offsets themselves, where the duplicate rule bites.
        for (const LinearGradientStop& stop : ramp.stops) {
            const rhi::Color expected = tessera::ui::paint::sample_gradient_stops(
                ramp.stops.data(), ramp.stops.size(), stop.offset);
            const rhi::Color actual =
                gradient::ramp(ramp.colors.data(), ramp.offsets.data(),
                               static_cast<std::uint32_t>(ramp.stops.size()), stop.offset);
            worst =
                std::max({worst, std::abs(actual.r - expected.r), std::abs(actual.g - expected.g),
                          std::abs(actual.b - expected.b), std::abs(actual.a - expected.a)});
        }
    }
    expect(compared > 10000, "the sweep compared many points");
    expect(worst <= 1.0e-6f, "the shader-shaped ramp equals sample_gradient_stops");
    std::cout << "gradient_math: ramp vs banded sampler over " << compared << " points, worst "
              << worst << "\n";
    expect(gradient::ramp(nullptr, nullptr, 0, 0.5f).a == 0.0f, "no stops is transparent");
}

gradient::Packed pack_of(const gradient::Fill& fill) {
    gradient::Packed packed;
    expect(gradient::pack(fill, packed), "the fill packs");
    return packed;
}

void linear_position() {
    const std::array<gradient::Stop, 2> stops{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.rect = {0.0f, 0.0f, 100.0f, 50.0f};
    fill.stops = stops;
    fill.angle_degrees = 90.0f; // to the right
    gradient::Packed packed = pack_of(fill);
    expect(near(gradient::position(packed.record, -50.0f, 0.0f), 0.0f), "90deg: left edge is 0");
    expect(near(gradient::position(packed.record, 50.0f, 0.0f), 1.0f), "90deg: right edge is 1");
    expect(near(gradient::position(packed.record, 0.0f, 20.0f), 0.5f), "90deg: the middle is 0.5");
    fill.angle_degrees = 180.0f; // to the bottom
    packed = pack_of(fill);
    expect(near(gradient::position(packed.record, 0.0f, -25.0f), 0.0f), "180deg: top edge is 0");
    expect(near(gradient::position(packed.record, 0.0f, 25.0f), 1.0f), "180deg: bottom edge is 1");
    fill.angle_degrees = 0.0f; // to the top
    packed = pack_of(fill);
    expect(near(gradient::position(packed.record, 0.0f, 25.0f), 0.0f), "0deg: bottom edge is 0");
    // The CSS magic corners: at 45deg the start and end colours sit exactly on the opposite
    // corners.
    fill.angle_degrees = 45.0f;
    packed = pack_of(fill);
    expect(near(gradient::position(packed.record, -50.0f, 25.0f), 0.0f, 1.0e-4f),
           "45deg: the bottom-left corner is 0");
    expect(near(gradient::position(packed.record, 50.0f, -25.0f), 1.0f, 1.0e-4f),
           "45deg: the top-right corner is 1");
}

void radial_position() {
    const std::array<gradient::Stop, 2> stops{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.kind = gradient::Kind::Radial;
    fill.rect = {10.0f, 10.0f, 100.0f, 60.0f}; // centre (60, 40)
    fill.center_x = 40.0f;                     // 20 px left of the quad centre
    fill.center_y = 40.0f;
    fill.radius_x = 40.0f;
    fill.radius_y = 20.0f;
    fill.stops = stops;
    const gradient::Packed packed = pack_of(fill);
    // Local points are pixels from the quad centre (60, 40); the gradient centre is (-20, 0) there.
    expect(near(gradient::position(packed.record, -20.0f, 0.0f), 0.0f), "the centre is 0");
    expect(near(gradient::position(packed.record, 20.0f, 0.0f), 1.0f), "one x radius out is 1");
    expect(near(gradient::position(packed.record, -20.0f, 20.0f), 1.0f), "one y radius out is 1");
    expect(near(gradient::position(packed.record, 0.0f, 0.0f), 0.5f), "half an x radius is 0.5");
    expect(gradient::position(packed.record, 50.0f, 25.0f) > 1.0f, "past the radius exceeds 1");
}

void conic_position() {
    const std::array<gradient::Stop, 2> stops{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.kind = gradient::Kind::Conic;
    fill.rect = {0.0f, 0.0f, 100.0f, 100.0f};
    fill.center_x = 50.0f;
    fill.center_y = 50.0f;
    fill.stops = stops;
    gradient::Packed packed = pack_of(fill);
    expect(near(gradient::position(packed.record, 0.0f, -10.0f), 0.0f), "12 o'clock is 0");
    expect(near(gradient::position(packed.record, 10.0f, 0.0f), 0.25f), "3 o'clock is a quarter");
    expect(near(gradient::position(packed.record, 0.0f, 10.0f), 0.5f), "6 o'clock is a half");
    expect(near(gradient::position(packed.record, -10.0f, 0.0f), 0.75f), "9 o'clock is 3 quarters");
    expect(near(gradient::position(packed.record, 0.0f, 0.0f), 0.0f), "the centre takes 0");
    // Just left of 12 o'clock is the end of the turn, not a negative angle.
    expect(gradient::position(packed.record, -0.01f, -10.0f) > 0.99f, "the seam wraps to 1");
    fill.from_angle_degrees = 90.0f; // the first stop moves to 3 o'clock
    packed = pack_of(fill);
    expect(near(gradient::position(packed.record, 10.0f, 0.0f), 0.0f),
           "from 90deg: 3 o'clock is 0");
    expect(near(gradient::position(packed.record, 0.0f, -10.0f), 0.75f),
           "from 90deg: 12 o'clock is 3 quarters");
}

void pack_refuses_what_cannot_be_drawn() {
    gradient::Packed packed;
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    std::array<gradient::Stop, 33> many{};
    gradient::Fill fill;
    fill.rect = {0.0f, 0.0f, 10.0f, 10.0f};
    expect(!gradient::pack(fill, packed), "no stops is refused");
    fill.stops = many;
    expect(!gradient::pack(fill, packed), "more than 32 stops is refused");
    fill.stops = two;
    expect(gradient::pack(fill, packed), "two stops pack");
    fill.rect = {0.0f, 0.0f, 0.0f, 10.0f};
    expect(!gradient::pack(fill, packed), "an empty quad is refused");
    fill.rect = {0.0f, 0.0f, 10.0f, 10.0f};
    fill.kind = gradient::Kind::Radial;
    fill.radius_x = 5.0f;
    fill.radius_y = 0.0f;
    expect(!gradient::pack(fill, packed), "a radial with a zero radius is refused");
    fill.radius_y = 5.0f;
    fill.center_x = std::numeric_limits<float>::quiet_NaN();
    expect(!gradient::pack(fill, packed), "a non-finite centre is refused");
}

void table_hands_out_records() {
    GradientTable table;
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    const std::array<gradient::Stop, 3> three{{{0.0f, kRed}, {0.5f, kGreen}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.rect = {0.0f, 0.0f, 10.0f, 10.0f};
    fill.stops = two;
    const auto first = table.add(fill);
    fill.stops = three;
    fill.kind = gradient::Kind::Conic;
    const auto second = table.add(fill);
    expect(first == 0u && second == 1u, "records are sequential");
    expect(table.records().size() == 2 && table.stop_count() == 5, "two records, five stops");
    expect(table.records()[0].first_stop == 0 && table.records()[1].first_stop == 2,
           "first_stop advances by the previous record's stop count");
    expect(table.records()[1].kind == static_cast<std::uint32_t>(gradient::Kind::Conic) &&
               table.records()[1].stop_count == 3,
           "the record carries its kind and count");
    expect(table.stop_colors().size() == 20 && table.stop_offsets().size() == 5,
           "four floats per stop colour, one per offset");
    // Straight alpha, untouched: the second record's middle stop is the translucent green.
    expect(near(table.stop_colors()[(2 + 1) * 4 + 3], 0.5f) &&
               near(table.stop_offsets()[2 + 1], 0.5f),
           "stops are stored as authored");
    table.reset();
    expect(table.records().empty() && table.stop_count() == 0, "reset empties the table");
    expect(table.add(fill) == 0u, "a reset table starts at record 0 again");
}

void table_stops_at_either_capacity() {
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.rect = {0.0f, 0.0f, 10.0f, 10.0f};
    fill.stops = two;
    GradientTable by_records;
    std::uint32_t accepted = 0;
    for (std::uint32_t index = 0; index < gradient::kMaxGradientRecords + 5; ++index) {
        accepted += by_records.add(fill).has_value() ? 1U : 0U;
    }
    expect(accepted == gradient::kMaxGradientRecords, "the record capacity is the limit");

    std::array<gradient::Stop, 32> widest{};
    for (std::size_t index = 0; index < widest.size(); ++index) {
        widest[index].offset = static_cast<float>(index) / 31.0f;
    }
    fill.stops = widest;
    GradientTable by_stops;
    accepted = 0;
    for (std::uint32_t index = 0; index < 100; ++index) {
        accepted += by_stops.add(fill).has_value() ? 1U : 0U;
    }
    expect(accepted == gradient::kMaxGradientStops / 32, "the stop capacity is the limit");
    // A refusal leaves the table exactly as it was.
    expect(by_stops.stop_count() == gradient::kMaxGradientStops, "a refusal adds nothing");
}

void layout_matches_the_documented_bytes() {
    expect(gradient::records_offset() == 53264, "the gradient area follows the clip area");
    expect(gradient::stop_colors_offset() == 53264 + 512 * 32, "colours follow the records");
    expect(gradient::stop_offsets_offset() == 53264 + 512 * 32 + 2048 * 16,
           "offsets follow the colours");
    expect(gradient::total_buffer_bytes() == 53264 + 512 * 32 + 2048 * 16 + 2048 * 4,
           "the buffer is the clip area plus 16 + 32 + 8 KiB");
}

} // namespace

int main() {
    ramp_matches_the_banded_sampler();
    linear_position();
    radial_position();
    conic_position();
    pack_refuses_what_cannot_be_drawn();
    table_hands_out_records();
    table_stops_at_either_capacity();
    layout_matches_the_documented_bytes();
    if (g_failures != 0) {
        std::cerr << "gradient_math: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "gradient_math: ok\n";
    return 0;
}
