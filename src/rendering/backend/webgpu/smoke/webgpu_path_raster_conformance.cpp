// SPDX-License-Identifier: MIT

// Emscripten-only conformance of the GPU path rasterizer on the WebGPU backend (issue #685,
// ADR-071, docs/design/gpu/compute.md section 3.1). Like webgpu_compute_conformance it needs a real
// WebGPU device, which the host injects as `Module.preinitializedWebGPUDevice`
// (scripts/webgpu_path_raster.mjs: Node with Dawn's `webgpu` bindings, JSPI).
//
// Corpus mode (default): every scene of tests/gpu/batched2d/path_raster_corpus.hpp goes through the
// production encoder and the four compute passes of shaders/path_raster.wgsl on the WebGPU device;
// the straight-alpha RGBA8 output is read back and compared with the lavapipe goldens in
// tests/golden/gpu/path_raster (the files the Vulkan golden test pins), per scene against the
// limits of the ThorVG comparison table of ADR-071. It reports the largest channel delta, the
// pixels over 4 and over 16 per scene, and fails only past the limit; an inexact match is printed,
// not hidden. The measurement mode (--measure) is webgpu_path_raster_measure.cpp.
//
//   webgpu_path_raster_conformance [--goldens <dir>] [--measure [--dense] [--frames N] [--blobs N]]

#include "path_raster_corpus.hpp"
#include "tessera/harness/image_compare.hpp"
#include "tessera/rendering/path/gpu_path_encoder.hpp"
#include "tessera/rendering/webgpu/path_raster_pipeline.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "webgpu_path_raster_smoke.hpp"
#include "webgpu_path_smoke_support.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace path = tessera::rendering::path;
namespace webgpu = tessera::rendering::webgpu;
namespace corpus = tessera::path_corpus;
using tessera::path_smoke::read_bytes;
using tessera::path_smoke::wait_for;

struct Limit {
    unsigned max;
    unsigned over4;
    unsigned over16;
};

// The enforced limits of the ThorVG comparison (max / over 4 / over 16 pixels), ADR-071 "Corpus,
// ThorVG comparison and goldens" and path_raster_thorvg_compare_tests.cpp. The WebGPU output is
// held to the same numbers against the lavapipe golden, which is a far tighter reference (the same
// algorithm) than ThorVG, so a pass here is a weak statement and the measured delta is the result.
const std::map<std::string, Limit>& limits() {
    static const std::map<std::string, Limit> table = {
        {"fill_star_non_zero", {4, 0, 0}},
        {"fill_star_even_odd", {4, 0, 0}},
        {"fill_nested_same_non_zero", {4, 0, 0}},
        {"fill_nested_same_even_odd", {4, 0, 0}},
        {"fill_nested_opposite_non_zero", {4, 0, 0}},
        {"fill_circle_alpha_overlap", {4, 0, 0}},
        {"fill_rect_fractional", {199, 385, 385}},
        {"stroke_cap_butt", {25, 57, 12}},
        {"stroke_cap_round", {30, 150, 42}},
        {"stroke_cap_square", {25, 49, 12}},
        {"stroke_join_miter", {8, 25, 0}},
        {"stroke_join_bevel", {8, 33, 0}},
        {"stroke_join_round", {20, 87, 0}},
        {"stroke_miter_exceeds_limit", {7, 10, 0}},
        {"stroke_closed_triangle_miter", {7, 19, 0}},
        {"stroke_open_triangle_butt", {7, 4, 0}},
        {"dash_even_butt", {7, 4, 0}},
        {"dash_odd_offset_round", {27, 288, 6}},
        {"dash_closed_polygon_butt", {65, 28, 4}},
        {"dash_curve_square", {78, 67, 6}},
        {"degenerate_zero_length_mid", {255, 55, 49}},
        {"degenerate_zero_length_dot", {255, 553, 553}},
        {"degenerate_collinear", {6, 0, 0}},
        {"degenerate_thin", {6, 0, 0}},
        {"degenerate_empty", {0, 0, 0}},
        {"degenerate_outside_target", {0, 0, 0}},
        {"degenerate_clipped_by_edge", {20, 22, 0}},
    };
    return table;
}

// One corpus scene through the production encoder and the four passes, straight-alpha RGBA8 out.
std::vector<std::uint8_t> render(rhi::Device& device, const path::GpuPathPipelineDescs& descs,
                                 const path::GpuPathScene& scene, std::uint32_t width,
                                 std::uint32_t height) {
    const path::EncodedGpuScene encoded = path::encode_gpu_scene(scene, width, height);
    const auto layout = path::plan_path_raster(encoded, width, height);
    if (!layout) {
        throw std::runtime_error("plan_path_raster refused the scene");
    }
    const std::size_t pixel_bytes = static_cast<std::size_t>(width) * height * 4U;
    if (layout->output_words * sizeof(std::uint32_t) < pixel_bytes) {
        throw std::runtime_error("the raster output is smaller than the target");
    }
    const auto storage = [&](const std::vector<std::uint32_t>& words, std::size_t minimum) {
        auto buffer = device.create_buffer(
            {std::max(words.size(), minimum) * sizeof(std::uint32_t), rhi::BufferUsage::Storage});
        if (!words.empty()) {
            buffer->write(words.data(), words.size() * sizeof(std::uint32_t));
        }
        return buffer;
    };
    const auto segs = storage(encoded.segs, path::kGpuSegWords);
    const auto paths = storage(encoded.paths, path::kGpuPathWords);
    const auto strokes = storage(encoded.strokes, path::kGpuStrokeWords);
    const auto dashes = storage(encoded.dashes, path::kGpuDashWords);
    rhi::BufferDesc read_write{layout->work_words * sizeof(std::uint32_t),
                               rhi::BufferUsage::Storage};
    read_write.gpu_read_write = true;
    const auto work = device.create_buffer(read_write);
    read_write.size = layout->output_words * sizeof(std::uint32_t);
    const auto output = device.create_buffer(read_write);
    const auto readback = device.create_buffer({pixel_bytes, rhi::BufferUsage::Readback});
    const path::PathRasterJob job_record = path::single_raster_job(*layout);
    const auto jobs = storage(std::vector<std::uint32_t>(path::kPathRasterJobWords, 0), 1);
    jobs->write(&job_record, sizeof(job_record));
    const std::uint32_t job_base = 0;

    const auto clear = device.create_compute_pipeline(descs.clear);
    const auto expand = device.create_compute_pipeline(descs.stroke);
    const auto bin = device.create_compute_pipeline(descs.bin);
    const auto fine = device.create_compute_pipeline(descs.fine);
    auto list = device.create_command_list();
    const auto bind_all = [&](const rhi::ComputePipeline& pipeline) {
        list->bind_compute_pipeline(pipeline);
        list->bind_compute_buffer(0, *segs);
        list->bind_compute_buffer(1, *paths);
        list->bind_compute_buffer(2, *work);
        list->bind_compute_buffer(3, *output);
        list->bind_compute_buffer(4, *strokes);
        list->bind_compute_buffer(5, *dashes);
        list->bind_compute_buffer(6, *jobs);
        list->compute_push_constants(&job_base, sizeof(job_base));
    };
    const auto groups = [](std::uint32_t count, std::uint32_t size) {
        return (count + size - 1) / size;
    };
    bind_all(*clear);
    list->dispatch(groups(layout->push.nodes_base, 256), 1, 1);
    list->compute_barrier();
    if (encoded.stroke_count > 0) {
        bind_all(*expand);
        list->dispatch(groups(encoded.stroke_count, 256), 1, 1);
        list->compute_barrier();
    }
    bind_all(*bin);
    list->dispatch(groups(std::max(encoded.seg_count, 1U), 256), 1, 1);
    list->compute_barrier();
    bind_all(*fine);
    list->dispatch(groups(width, 16), groups(height, 16), 1);
    list->copy_buffer(*output, *readback, pixel_bytes);
    list->submit_async();
    if (!wait_for([&] { return list->is_complete(); })) {
        throw std::runtime_error("the submission did not complete");
    }
    return read_bytes(*readback, pixel_bytes);
}

struct Diff {
    unsigned max = 0;
    unsigned over4 = 0;
    unsigned over16 = 0;
    unsigned differing = 0;   // pixels whose straight-alpha bytes are not identical
    std::string worst_pixels; // up to 4 pixels over 16, for the failure log
};

// Premultiplied channel of a straight-alpha pixel, rounded: the ADR-071 comparison measures what a
// draw would composite, and a pixel whose alpha rounds to 0 carries no colour whatever its RGB
// bytes say (the lavapipe goldens keep the RGB of a coverage under 1/510; another device's rounding
// may not).
unsigned premultiplied(const std::vector<std::uint8_t>& pixels, std::size_t px, std::size_t c) {
    return c == 3 ? pixels[px + 3] : (pixels[px + c] * pixels[px + 3] + 127U) / 255U;
}

Diff diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    Diff result;
    for (std::size_t px = 0; px + 3 < a.size(); px += 4) {
        unsigned worst = 0;
        for (std::size_t c = 0; c < 4; ++c) {
            const int delta = static_cast<int>(premultiplied(a, px, c)) -
                              static_cast<int>(premultiplied(b, px, c));
            worst = std::max(worst, static_cast<unsigned>(delta < 0 ? -delta : delta));
        }
        result.max = std::max(result.max, worst);
        result.differing += std::memcmp(&a[px], &b[px], 4) != 0 ? 1U : 0U;
        result.over4 += worst > 4 ? 1U : 0U;
        result.over16 += worst > 16 ? 1U : 0U;
        if (worst > 16 && result.over16 <= 4) {
            char text[160];
            std::snprintf(text, sizeof(text), " (%zu,%zu) got %u,%u,%u,%u want %u,%u,%u,%u;",
                          (px / 4) % 96, (px / 4) / 96, a[px], a[px + 1], a[px + 2], a[px + 3],
                          b[px], b[px + 1], b[px + 2], b[px + 3]);
            result.worst_pixels += text;
        }
    }
    return result;
}

std::vector<std::uint8_t> read_file(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

int run_corpus(rhi::Device& device, const std::string& golden_dir) {
    constexpr std::uint32_t kSize = corpus::kCorpusSize;
    const path::GpuPathPipelineDescs descs = webgpu::path_raster_pipeline_descs();
    int failures = 0;
    unsigned exact = 0;
    unsigned scenes = 0;
    unsigned nondeterministic = 0;
    std::printf("%-32s %5s %9s %6s %7s  %s\n", "scene", "max", "differing", "over4", "over16",
                "limit (max/over4/over16), max and over* premultiplied");
    for (const corpus::Scene& scene : corpus::make_corpus()) {
        const std::vector<std::uint8_t> first = render(device, descs, scene.gpu, kSize, kSize);
        const std::vector<std::uint8_t> second = render(device, descs, scene.gpu, kSize, kSize);
        if (first.size() != kSize * kSize * 4U || second.size() != first.size()) {
            std::fprintf(stderr, "FAIL %s: readback failed\n", scene.name.c_str());
            ++failures;
            continue;
        }
        // Nodes reach a row's list in the order the threads win an atomic, so a GPU may sum a
        // pixel's segments in a different order run to run (lavapipe does not): the bytes can
        // differ in the last bit, never by more than 1 once premultiplied.
        if (const Diff again = diff(first, second); again.max > 1) {
            std::fprintf(stderr, "FAIL %s: two renders differ by %u\n", scene.name.c_str(),
                         again.max);
            ++failures;
            continue;
        } else if (again.differing != 0) {
            ++nondeterministic;
        }
        bool any_ink = false;
        for (std::size_t i = 3; i < first.size(); i += 4) {
            any_ink = any_ink || first[i] != 0;
        }
        if (any_ink == scene.expect_blank) {
            std::fprintf(stderr, "FAIL %s: blank/ink expectation\n", scene.name.c_str());
            ++failures;
        }
        const auto golden =
            tessera::harness::decode_png(read_file(golden_dir + "/" + scene.name + ".png"));
        if (!golden || golden->width != kSize || golden->height != kSize) {
            std::fprintf(stderr, "FAIL %s: golden missing or wrong size (%s)\n", scene.name.c_str(),
                         golden_dir.c_str());
            ++failures;
            continue;
        }
        const Diff d = diff(first, golden->rgba);
        const auto limit = limits().find(scene.name);
        const bool known = limit != limits().end();
        const bool ok = known && d.max <= limit->second.max && d.over4 <= limit->second.over4 &&
                        d.over16 <= limit->second.over16;
        ++scenes;
        exact += d.differing == 0 ? 1U : 0U;
        std::printf("%-32s %5u %9u %6u %7u  %u/%u/%u %s\n", scene.name.c_str(), d.max, d.differing,
                    d.over4, d.over16, known ? limit->second.max : 0,
                    known ? limit->second.over4 : 0, known ? limit->second.over16 : 0,
                    ok ? "" : "FAIL");
        if (!ok && !d.worst_pixels.empty()) {
            std::printf("    over 16:%s\n", d.worst_pixels.c_str());
        }
        failures += ok ? 0 : 1;
    }
    std::printf("webgpu_path_raster_conformance: %u scene(s), %u bit-identical to the lavapipe "
                "golden, %u not bit-identical between two renders\n",
                scenes, exact, nondeterministic);
    return failures == 0 ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    std::string goldens = "tests/golden/gpu/path_raster";
    tessera::path_smoke::MeasureOptions measure;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--goldens" && i + 1 < argc) {
            goldens = argv[++i];
        } else if (arg == "--measure") {
            measure.enabled = true;
        } else if (arg == "--dense") {
            measure.dense = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            measure.frames = std::atoi(argv[++i]);
        } else if (arg == "--blobs" && i + 1 < argc) {
            measure.blobs = std::atoi(argv[++i]);
        } else {
            std::fprintf(stderr, "webgpu_path_raster_conformance: unknown argument %s\n",
                         arg.c_str());
            return 2;
        }
    }
    try {
        WGPUDevice wgpu_device = emscripten_webgpu_get_device();
        WGPUQueue queue = wgpuDeviceGetQueue(wgpu_device);
        auto device = webgpu::create_device(wgpu_device, queue);
        if (!webgpu::register_path_raster_shaders(*device)) {
            std::fprintf(stderr, "webgpu_path_raster_conformance: register_shader failed\n");
            return 1;
        }
        if (!device->supports_compute()) {
            std::fprintf(stderr, "webgpu_path_raster_conformance: the device has no compute\n");
            return 1;
        }
        const int code = measure.enabled ? tessera::path_smoke::run_measure(*device, queue, measure)
                                         : run_corpus(*device, goldens);
        if (code != 0) {
            return code;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "webgpu_path_raster_conformance: unexpected exception: %s\n",
                     error.what());
        return 3;
    }
    std::puts("webgpu_path_raster_conformance: ok");
    return 0;
}
