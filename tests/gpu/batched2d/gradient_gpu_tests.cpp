// SPDX-License-Identifier: MIT
//
// Per-fragment gradients on a real device (docs/design/gpu/gradients.md, ADR-076): one quad per
// gradient layer, evaluated exactly at every pixel centre by the fragment shader.
//
// Evidence:
//  - every gradient kind (linear, radial, conic) reads back within 2/255 of the CPU twin, which
//  packs
//    the same record and evaluates it with the shader's own expressions (gradient_math.hpp);
//  - the ramp rules the banded path had survive: a duplicate offset is a hard edge, a stop colour
//    outside the ramp is held, a 32-stop repeating ramp is followed, a translucent ramp composites
//    once;
//  - a gradient under a transform rotates with its box (the record is quad-local);
//  - a rounded clip cuts the corners, the flat-layer opacity scales the ramp;
//  - many gradients are one draw call and one buffer upload, the record table's limit falls back
//    instead of failing, and two frames in flight never read each other's records;
//  - the run is validation-clean.
//
// Skips cleanly when the machine has no Vulkan device.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/gradient_math.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gradient = tessera::rendering::gpu::gradient;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;

constexpr std::uint32_t kSize = 64;
constexpr int kTolerance = 2; // out of 255

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gradient_gpu: " << message << "\n";
    }
    return condition;
}

struct Harness {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Texture> white;
};

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.load_op = rhi::LoadOp::Clear;
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

std::vector<std::uint8_t> read_target(rhi::Device& device, rhi::RenderTarget& target) {
    const std::size_t bytes = static_cast<std::size_t>(kSize) * kSize * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, rhi::BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

std::array<int, 3> pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                            std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * sizeof(std::uint32_t);
    return {pixels[index], pixels[index + 1], pixels[index + 2]};
}

// One frame: `draw` records into a fresh renderer with AA off (hard edges, exact bytes). The
// counters are read before the renderer goes away.
struct Frame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t quads = 0;
    std::uint32_t fallbacks = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t vertices = 0;
};

Frame render(Harness& harness, const std::function<void(Batched2DRenderer&)>& draw) {
    auto target = harness.device->create_render_target(target_desc());
    auto commands = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    Frame frame;
    {
        Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config);
        renderer.begin(*commands, *target, target_desc());
        draw(renderer);
        frame.vertices = renderer.vertex_count();
        renderer.end();
        commands->submit();
        frame.quads = renderer.gradient_quads();
        frame.fallbacks = renderer.gradient_fallbacks();
        frame.draw_calls = renderer.draw_calls();
    }
    frame.pixels = read_target(*harness.device, *target);
    return frame;
}

// The CPU twin of one fill over black at pixel (x, y): the straight-alpha ramp colour blended into
// an opaque black frame, quantised like the target.
std::array<int, 3> twin_pixel(const gradient::Fill& fill, std::uint32_t x, std::uint32_t y,
                              float opacity = 1.0f) {
    gradient::Packed packed;
    if (!gradient::pack(fill, packed)) {
        return {0, 0, 0};
    }
    const float centre_x = fill.rect.x + fill.rect.width * 0.5f;
    const float centre_y = fill.rect.y + fill.rect.height * 0.5f;
    const rhi::Color color = gradient::evaluate(packed, static_cast<float>(x) + 0.5f - centre_x,
                                                static_cast<float>(y) + 0.5f - centre_y);
    const float alpha = std::clamp(color.a * opacity, 0.0f, 1.0f);
    const auto byte = [alpha](float channel) {
        return static_cast<int>(std::lround(std::clamp(channel * alpha, 0.0f, 1.0f) * 255.0f));
    };
    return {byte(color.r), byte(color.g), byte(color.b)};
}

bool inside(const rhi::Rect& rect, std::uint32_t x, std::uint32_t y) {
    const float cx = static_cast<float>(x) + 0.5f;
    const float cy = static_cast<float>(y) + 0.5f;
    return cx >= rect.x && cx < rect.x + rect.width && cy >= rect.y && cy < rect.y + rect.height;
}

// Compares every pixel of a frame with the twin: pixels inside `rect` against the fill, pixels
// outside against black. Reports the worst channel delta.
int worst_against_twin(const Frame& frame, const gradient::Fill& fill, float opacity = 1.0f) {
    int worst = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const auto expected = inside(fill.rect, x, y) ? twin_pixel(fill, x, y, opacity)
                                                          : std::array<int, 3>{0, 0, 0};
            const auto actual = pixel_at(frame.pixels, x, y);
            for (int channel = 0; channel < 3; ++channel) {
                worst = std::max(worst, std::abs(actual[channel] - expected[channel]));
            }
        }
    }
    return worst;
}

constexpr rhi::Color kRed{1.0f, 0.0f, 0.0f, 1.0f};
constexpr rhi::Color kGreen{0.0f, 1.0f, 0.0f, 1.0f};
constexpr rhi::Color kBlue{0.0f, 0.0f, 1.0f, 1.0f};
constexpr rhi::Color kYellow{1.0f, 1.0f, 0.0f, 1.0f};
constexpr rhi::Rect kBox{8.0f, 8.0f, 48.0f, 48.0f};

bool kind_matches_twin(Harness& harness, const char* name, const gradient::Fill& fill) {
    const Frame frame =
        render(harness, [&](Batched2DRenderer& renderer) { (void)renderer.push_gradient(fill); });
    const int worst = worst_against_twin(frame, fill);
    bool ok = expect(frame.quads == 1 && frame.fallbacks == 0 && frame.vertices == 6 &&
                         frame.draw_calls == 1,
                     name);
    if (worst > kTolerance) {
        std::cerr << "gradient_gpu: " << name << " is " << worst << "/255 from the CPU twin\n";
        for (const std::uint32_t at : {2u, 10u, 20u, 32u, 44u, 54u}) {
            const auto actual = pixel_at(frame.pixels, at, 32);
            const auto expected =
                inside(fill.rect, at, 32) ? twin_pixel(fill, at, 32) : std::array<int, 3>{0, 0, 0};
            std::cerr << "  (" << at << ",32) got " << actual[0] << "," << actual[1] << ","
                      << actual[2] << " expected " << expected[0] << "," << expected[1] << ","
                      << expected[2] << "\n";
        }
        ok = false;
    }
    std::cout << "gradient_gpu: " << name << " worst against the CPU twin: " << worst << "/255\n";
    return ok;
}

bool every_kind(Harness& harness) {
    bool ok = true;
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill linear;
    linear.rect = kBox;
    linear.angle_degrees = 90.0f;
    linear.stops = two;
    ok = kind_matches_twin(harness, "linear 90deg", linear) && ok;

    // An oblique angle on a non-square box: the case the dominant-axis bands only approximated.
    const std::array<gradient::Stop, 3> three{
        {{0.0f, kRed}, {0.35f, kGreen}, {1.0f, rhi::Color{0.0f, 0.0f, 1.0f, 0.25f}}}};
    gradient::Fill oblique;
    oblique.rect = {6.5f, 10.0f, 51.0f, 40.0f};
    oblique.angle_degrees = 63.0f;
    oblique.stops = three;
    ok = kind_matches_twin(harness, "linear 63deg, three stops, translucent end", oblique) && ok;

    // Closest-side on an off-centre ellipse: the box corners lie beyond the last stop, which CSS
    // fills with the last colour, and the banded grid left unpainted.
    gradient::Fill radial;
    radial.kind = gradient::Kind::Radial;
    radial.rect = kBox;
    radial.center_x = 24.0f;
    radial.center_y = 30.0f;
    radial.radius_x = 16.0f;
    radial.radius_y = 22.0f;
    radial.stops = two;
    ok = kind_matches_twin(harness, "radial ellipse, corners past the last stop", radial) && ok;

    // A duplicate offset is a hard edge: the later stop wins at 0.5.
    const std::array<gradient::Stop, 4> hard{
        {{0.0f, kRed}, {0.5f, kRed}, {0.5f, kBlue}, {1.0f, kBlue}}};
    gradient::Fill conic;
    conic.kind = gradient::Kind::Conic;
    conic.rect = kBox;
    conic.center_x = 29.5f;
    conic.center_y = 35.5f;
    conic.from_angle_degrees = 30.0f;
    conic.stops = hard;
    ok = kind_matches_twin(harness, "conic with a hard edge, from 30deg", conic) && ok;

    // The widest ramp a record holds: a 32-stop sawtooth (the cap; a repeating gradient no longer
    // needs it).
    std::array<gradient::Stop, 32> saw{};
    for (std::size_t index = 0; index < saw.size(); ++index) {
        saw[index].offset = static_cast<float>(index) / 31.0f;
        saw[index].color = index % 2 == 0 ? kYellow : kGreen;
    }
    gradient::Fill repeating;
    repeating.rect = kBox;
    repeating.angle_degrees = 200.0f;
    repeating.stops = saw;
    ok = kind_matches_twin(harness, "linear, 32-stop repeating ramp", repeating) && ok;

    // A repeating ramp is wrapped by the shader, not expanded into stops (short period, radial,
    // conic), as is a ramp whose stops sit outside the line.
    const std::array<gradient::Stop, 2> cycle{{{0.1f, kYellow}, {0.15f, kGreen}}};
    gradient::Fill wrapped;
    wrapped.rect = kBox;
    wrapped.angle_degrees = 200.0f;
    wrapped.repeating = true;
    wrapped.stops = cycle;
    ok = kind_matches_twin(harness, "linear, two-stop repeating ramp wrapped in the shader",
                           wrapped) &&
         ok;
    gradient::Fill wrapped_radial = radial;
    wrapped_radial.repeating = true;
    wrapped_radial.stops = cycle;
    ok = kind_matches_twin(harness, "radial, repeating ramp", wrapped_radial) && ok;
    gradient::Fill wrapped_conic = conic;
    wrapped_conic.repeating = true;
    const std::array<gradient::Stop, 2> sweep{{{0.0f, kRed}, {0.125f, kBlue}}};
    wrapped_conic.stops = sweep;
    ok = kind_matches_twin(harness, "conic, repeating ramp", wrapped_conic) && ok;
    const std::array<gradient::Stop, 3> wide{{{-0.4f, kRed}, {0.5f, kGreen}, {1.6f, kBlue}}};
    gradient::Fill beyond;
    beyond.rect = kBox;
    beyond.angle_degrees = 63.0f;
    beyond.stops = wide;
    ok = kind_matches_twin(harness, "linear, stops outside the line", beyond) && ok;

    // One stop is a flat fill; two stops with the first past 0 hold the first colour before it.
    const std::array<gradient::Stop, 1> one{{{0.5f, kGreen}}};
    gradient::Fill flat;
    flat.rect = kBox;
    flat.stops = one;
    ok = kind_matches_twin(harness, "a single stop", flat) && ok;
    const std::array<gradient::Stop, 2> inset{{{0.4f, kRed}, {0.6f, kBlue}}};
    gradient::Fill held;
    held.rect = kBox;
    held.angle_degrees = 90.0f;
    held.stops = inset;
    ok = kind_matches_twin(harness, "end stops held outside the ramp", held) && ok;
    return ok;
}

// A rotation about the box centre: the record is quad-local, so the ramp turns with the box. The
// expected colour comes from inverse-mapping the pixel centre; pixels within a pixel and a half of
// the rotated edge are skipped (the quad's own coverage there is the rasteriser's, not the ramp's).
bool follows_a_transform(Harness& harness) {
    constexpr float kAngle = 0.5235988f; // 30 degrees
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    gradient::Fill fill;
    fill.rect = {16.0f, 20.0f, 32.0f, 24.0f};
    fill.angle_degrees = 90.0f;
    fill.stops = two;
    const float cx = 32.0f;
    const float cy = 32.0f;
    const float c = std::cos(kAngle);
    const float s = std::sin(kAngle);
    rhi::Mat3 transform = rhi::Mat3::identity();
    // Rotation about (cx, cy): translate, rotate, translate back (row-major 3x3, see rhi::Mat3).
    transform.m[0] = c;
    transform.m[1] = -s;
    transform.m[2] = cx - c * cx + s * cy;
    transform.m[3] = s;
    transform.m[4] = c;
    transform.m[5] = cy - s * cx - c * cy;
    const Frame frame = render(harness, [&](Batched2DRenderer& renderer) {
        renderer.push_transform(transform);
        (void)renderer.push_gradient(fill);
        renderer.pop_transform();
    });
    int worst = 0;
    int checked = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            // Inverse rotation of the pixel centre back into the box's own frame.
            const float px = static_cast<float>(x) + 0.5f - cx;
            const float py = static_cast<float>(y) + 0.5f - cy;
            const float lx = c * px + s * py;
            const float ly = -s * px + c * py;
            const float edge = std::min(fill.rect.width * 0.5f - std::abs(lx),
                                        fill.rect.height * 0.5f - std::abs(ly));
            if (edge < 1.5f) {
                continue;
            }
            // The box's centre is (32, 32), so (lx, ly) is already the quad-local point.
            gradient::Packed packed;
            (void)gradient::pack(fill, packed);
            const rhi::Color color = gradient::evaluate(packed, lx, ly);
            const auto actual = pixel_at(frame.pixels, x, y);
            worst = std::max(
                {worst, std::abs(actual[0] - static_cast<int>(std::lround(color.r * 255.0f))),
                 std::abs(actual[2] - static_cast<int>(std::lround(color.b * 255.0f)))});
            ++checked;
        }
    }
    bool ok = expect(checked > 400, "the rotated box has interior pixels to compare");
    ok = expect(worst <= kTolerance + 1, "a rotated gradient turns with its box") && ok;
    std::cout << "gradient_gpu: rotated 30deg worst against the inverse-mapped ramp: " << worst
              << "/255 over " << checked << " pixels\n";
    return ok;
}

bool rounded_clip_and_flat_opacity(Harness& harness) {
    const std::array<gradient::Stop, 2> two{{{0.0f, kGreen}, {1.0f, kGreen}}};
    gradient::Fill fill;
    fill.rect = kBox;
    fill.stops = two;
    // A rounded clip cuts the corner pixels and keeps the interior.
    const Frame clipped = render(harness, [&](Batched2DRenderer& renderer) {
        renderer.push_clip(tessera::rendering::gpu::clip::uniform_rect(kBox, 16.0f));
        (void)renderer.push_gradient(fill);
        renderer.pop_clip();
    });
    bool ok = expect(pixel_at(clipped.pixels, 32, 32)[1] == 255, "the clipped interior is painted");
    ok = expect(pixel_at(clipped.pixels, 9, 9)[1] == 0, "the rounded corner is cut") && ok;
    ok = expect(pixel_at(clipped.pixels, 8, 32)[1] == 255, "the clip edge keeps the ramp") && ok;

    // A flat layer folds its opacity into every colour, a gradient quad included.
    const Frame faded = render(harness, [&](Batched2DRenderer& renderer) {
        renderer.push_layer(kBox, 0.5f);
        (void)renderer.push_gradient(fill);
        renderer.pop_layer();
    });
    ok = expect(std::abs(pixel_at(faded.pixels, 32, 32)[1] - 128) <= kTolerance,
                "a flat layer at 0.5 halves the ramp") &&
         ok;
    return ok;
}

// Many gradients are one draw call: the records ride in one buffer and the quads share a batch.
bool batches_into_one_draw(Harness& harness) {
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    constexpr int kCount = 100;
    const Frame frame = render(harness, [&](Batched2DRenderer& renderer) {
        for (int index = 0; index < kCount; ++index) {
            gradient::Fill fill;
            fill.rect = {static_cast<float>(index % 10) * 6.0f,
                         static_cast<float>(index / 10) * 6.0f, 5.0f, 5.0f};
            fill.angle_degrees = static_cast<float>(index) * 3.0f;
            fill.stops = two;
            (void)renderer.push_gradient(fill);
        }
    });
    bool ok = expect(frame.quads == kCount && frame.fallbacks == 0, "every gradient is a quad");
    ok = expect(frame.vertices == 6u * kCount && frame.draw_calls == 1,
                "one hundred gradients are 600 vertices in one draw call") &&
         ok;
    // Spot-check that records are not mixed up: the first and the last box.
    const auto first_actual = pixel_at(frame.pixels, 0, 2);
    const auto last_actual = pixel_at(frame.pixels, 54, 56);
    ok = expect(first_actual[0] + first_actual[2] > 150, "the first box shows its ramp") && ok;
    ok = expect(last_actual[0] + last_actual[2] > 150, "the last box shows its ramp") && ok;
    return ok;
}

// A frame with more records than the table holds paints the rest banded instead of failing.
bool table_limit_falls_back(Harness& harness) {
    const std::array<gradient::Stop, 2> two{{{0.0f, kRed}, {1.0f, kBlue}}};
    std::uint32_t accepted = 0;
    std::uint32_t refused = 0;
    const Frame frame = render(harness, [&](Batched2DRenderer& renderer) {
        for (std::uint32_t index = 0; index < gradient::kMaxGradientRecords + 3; ++index) {
            gradient::Fill fill;
            fill.rect = {1.0f, 1.0f, 2.0f, 2.0f};
            fill.stops = two;
            (renderer.push_gradient(fill) ? accepted : refused) += 1;
        }
    });
    bool ok = expect(accepted == gradient::kMaxGradientRecords && refused == 3,
                     "the record limit refuses, never throws");
    ok = expect(frame.fallbacks == 3, "every refusal is counted") && ok;
    return ok;
}

// Two frames in flight: the second frame's records must not overwrite what the first still reads.
bool frames_in_flight(Harness& harness) {
    const std::array<gradient::Stop, 2> red{{{0.0f, kRed}, {1.0f, kRed}}};
    const std::array<gradient::Stop, 2> blue{{{0.0f, kBlue}, {1.0f, kBlue}}};
    auto target_a = harness.device->create_render_target(target_desc());
    auto target_b = harness.device->create_render_target(target_desc());
    auto commands_a = harness.device->create_command_list();
    auto commands_b = harness.device->create_command_list();
    Batched2DConfig config;
    config.enable_aa = false;
    config.golden_mode = true;
    config.frames_in_flight = 2;
    Batched2DRenderer renderer(*harness.device, *harness.pipeline, *harness.white, config);
    gradient::Fill fill;
    fill.rect = kBox;
    // Frame A is submitted before frame B is recorded, as the renderer's contract says: the record
    // buffer is one storage descriptor, so a command list reads whatever is bound when it executes.
    renderer.begin(*commands_a, *target_a, target_desc());
    fill.stops = red;
    (void)renderer.push_gradient(fill);
    renderer.end();
    commands_a->submit();
    renderer.begin(*commands_b, *target_b, target_desc());
    fill.stops = blue;
    (void)renderer.push_gradient(fill);
    renderer.end();
    commands_b->submit();
    const auto a = read_target(*harness.device, *target_a);
    const auto b = read_target(*harness.device, *target_b);
    bool ok = expect(pixel_at(a, 32, 32)[0] == 255 && pixel_at(a, 32, 32)[2] == 0,
                     "frame A kept its own record");
    ok = expect(pixel_at(b, 32, 32)[2] == 255 && pixel_at(b, 32, 32)[0] == 0,
                "frame B drew its own record") &&
         ok;
    return ok;
}

} // namespace

int main() {
    rhi::DeviceDesc device_desc;
    device_desc.backend = rhi::Backend::Vulkan;
    device_desc.validation = true;

    Harness harness;
    try {
        harness.device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "gradient_gpu: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }
    if (!harness.device->supports_analytic_gradients()) {
        std::cout << "gradient_gpu: skipped (device reports no analytic gradients)\n";
        return 0;
    }
    harness.pipeline = harness.device->create_pipeline(
        tessera::rendering::vulkan::batched2d_pipeline_desc(rhi::TextureFormat::R8G8B8A8Unorm));
    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    harness.white = harness.device->create_texture(white_desc);

    bool ok = every_kind(harness);
    ok = follows_a_transform(harness) && ok;
    ok = rounded_clip_and_flat_opacity(harness) && ok;
    ok = batches_into_one_draw(harness) && ok;
    ok = table_limit_falls_back(harness) && ok;
    ok = frames_in_flight(harness) && ok;

    const std::uint64_t validation_messages =
        tessera::rendering::vulkan::validation_message_count();
    ok = expect(validation_messages == 0, "the run produced no validation messages") && ok;
    if (!ok) {
        std::cerr << "gradient_gpu: failed (validation=" << validation_messages << ")\n";
        return 1;
    }
    std::cout << "gradient_gpu: ok\n";
    return 0;
}
