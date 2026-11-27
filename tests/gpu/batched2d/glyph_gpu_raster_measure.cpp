// SPDX-License-Identifier: MIT
//
// Issue #686 step 2: glyph outlines through the GPU path rasterizer (#685), next to the FreeType
// numbers of docs/design/text-gpu-raster.md. Not a test (no ctest entry); run by hand in Release:
//   glyph_gpu_raster_measure <font.ttf>
// The 95 printable ASCII glyphs are decomposed from the font's outlines into one scale-independent
// document each (em = 64 units, so a pixel size S is the scale S / 64), then rasterized into S x S
// textures at 128, 256 and 512 px. Two shapes of work are timed: the whole set in one batch (the
// atlas rebuild on a scale step) and one glyph per flush (a single new glyph). CPU is the time the
// UI thread spends in enqueue + flush; GPU is the time until the batch has finished. The encode of
// a document is paid once per glyph and serves every size.

#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace rhi = tessera::rendering::rhi;
namespace path = tessera::rendering::path;
namespace vk = tessera::rendering::vulkan;

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kEm = 64.0f;
constexpr float kBaseline = 52.0f;
constexpr float kLeft = 6.0f;

[[nodiscard]] double micros(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration<double, std::micro>(to - from).count();
}

[[nodiscard]] double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

struct Decomposer {
    path::Path path;
    float unit = 1.0f;
    [[nodiscard]] path::PointF point(const FT_Vector* v) const {
        return {kLeft + static_cast<float>(v->x) * unit,
                kBaseline - static_cast<float>(v->y) * unit};
    }
};

int move_to(const FT_Vector* to, void* user) {
    auto& d = *static_cast<Decomposer*>(user);
    if (!d.path.segments.empty()) {
        d.path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
    }
    d.path.segments.push_back({path::SegmentKind::move_to, d.point(to), {}, {}});
    return 0;
}
int line_to(const FT_Vector* to, void* user) {
    auto& d = *static_cast<Decomposer*>(user);
    d.path.segments.push_back({path::SegmentKind::line_to, d.point(to), {}, {}});
    return 0;
}
int conic_to(const FT_Vector* control, const FT_Vector* to, void* user) {
    auto& d = *static_cast<Decomposer*>(user);
    d.path.segments.push_back({path::SegmentKind::quad_to, d.point(control), d.point(to), {}});
    return 0;
}
int cubic_to(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to, void* user) {
    auto& d = *static_cast<Decomposer*>(user);
    d.path.segments.push_back({path::SegmentKind::cubic_to, d.point(c1), d.point(c2), d.point(to)});
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: glyph_gpu_raster_measure <font.ttf>\n", stderr);
        return 2;
    }
    try {
        FT_Library library = nullptr;
        FT_Face face = nullptr;
        if (FT_Init_FreeType(&library) != 0 || FT_New_Face(library, argv[1], 0, &face) != 0) {
            std::fputs("glyph_gpu_raster_measure: cannot open the font\n", stderr);
            return 2;
        }
        const float unit = kEm / static_cast<float>(face->units_per_EM);
        std::vector<std::vector<path::PathShape>> glyphs;
        for (char32_t c = 0x21; c <= 0x7E; ++c) { // 94 glyphs; the space has no outline
            const FT_UInt index = FT_Get_Char_Index(face, c);
            if (FT_Load_Glyph(face, index, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP) != 0) {
                continue;
            }
            Decomposer decomposer;
            decomposer.unit = unit / 1.0f;
            const FT_Outline_Funcs funcs{move_to, line_to, conic_to, cubic_to, 0, 0};
            FT_Outline_Decompose(&face->glyph->outline, &funcs, &decomposer);
            decomposer.path.segments.push_back({path::SegmentKind::close, {}, {}, {}});
            path::PathShape shape;
            shape.path = std::move(decomposer.path);
            shape.paint.fill = path::Rgba8{0, 0, 0, 255};
            glyphs.push_back({std::move(shape)});
        }

        rhi::DeviceDesc desc;
        desc.backend = rhi::Backend::Vulkan;
        auto device = tessera::rendering::vulkan::create_vulkan_device(desc);
        if (!path::GpuPathRasterizer::supported(*device)) {
            std::puts("glyph_gpu_raster_measure: no compute or buffer-to-texture copy");
            return 0;
        }
        path::GpuPathRasterizerConfig config;
        config.max_jobs_per_batch = 128;
        config.max_pooled_bytes = 2048ULL * 1024ULL * 1024ULL;
        path::GpuPathRasterizer rasterizer(*device, vk::path_raster_pipeline_descs(), config);
        (void)rasterizer.warm(2);

        std::printf("%zu glyphs from %s\n", glyphs.size(), argv[1]);
        std::printf(
            "size  encode us/glyph   batch: CPU us/glyph  GPU us/glyph   single: CPU us  GPU us\n");
        for (const std::uint32_t size : {128U, 256U, 512U}) {
            const float scale = static_cast<float>(size) / kEm;
            std::vector<std::shared_ptr<const path::GpuPathDocument>> documents;
            const auto e0 = Clock::now();
            for (const auto& glyph : glyphs) {
                documents.push_back(path::make_gpu_document(glyph, std::max(scale, 1.0f)));
            }
            const double encode_us = micros(e0, Clock::now()) / static_cast<double>(glyphs.size());

            std::vector<std::unique_ptr<rhi::Texture>> textures;
            for (std::size_t i = 0; i < glyphs.size(); ++i) {
                rhi::TextureDesc texture_desc;
                texture_desc.extent = {size, size};
                textures.push_back(device->create_texture(texture_desc));
            }

            std::vector<double> batch_cpu;
            std::vector<double> batch_gpu;
            for (int run = 0; run < 12; ++run) {
                const auto t0 = Clock::now();
                for (std::size_t i = 0; i < glyphs.size(); ++i) {
                    if (!rasterizer.enqueue(documents[i], scale, *textures[i], {size, size})
                             .has_value()) {
                        std::puts("enqueue refused");
                        return 1;
                    }
                }
                (void)rasterizer.flush();
                const auto t1 = Clock::now();
                device->wait_idle();
                const auto t2 = Clock::now();
                rasterizer.collect();
                if (run >= 2) {
                    batch_cpu.push_back(micros(t0, t1) / static_cast<double>(glyphs.size()));
                    batch_gpu.push_back(micros(t1, t2) / static_cast<double>(glyphs.size()));
                }
            }

            std::vector<double> one_cpu;
            std::vector<double> one_gpu;
            for (int run = 0; run < 3; ++run) {
                for (std::size_t i = 0; i < glyphs.size(); ++i) {
                    const auto t0 = Clock::now();
                    (void)rasterizer.enqueue(documents[i], scale, *textures[i], {size, size});
                    (void)rasterizer.flush();
                    const auto t1 = Clock::now();
                    device->wait_idle();
                    const auto t2 = Clock::now();
                    rasterizer.collect();
                    if (run >= 1) {
                        one_cpu.push_back(micros(t0, t1));
                        one_gpu.push_back(micros(t1, t2));
                    }
                }
            }
            std::printf("%4u  %14.1f   %18.1f  %12.1f   %14.1f  %6.1f\n", size, encode_us,
                        median(batch_cpu), median(batch_gpu), median(one_cpu), median(one_gpu));
        }
        FT_Done_Face(face);
        FT_Done_FreeType(library);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "glyph_gpu_raster_measure: %s\n", error.what());
        return 2;
    }
    return 0;
}
