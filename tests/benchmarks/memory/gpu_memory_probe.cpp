// SPDX-License-Identifier: MIT
//
// gpu_memory_probe (issue #703): the GPU half of the memory budget, as `name=value` rows for
// scripts/check_memory_budget.py.
//
// Why this is a separate probe from `memory_sizes_probe`: that one links no RHI target on purpose
// (it must run on every lane, including ones with no GPU backend), and this one needs a real
// `rhi::Device`. Keeping them apart means a lane without Vulkan still measures every CPU row.
//
// Why the numbers are deterministic even though a GPU is involved: the category bytes are *our*
// accounting, computed from each resource's descriptor (extent x texel size, buffer size), not read
// back from the driver's block sizes. lavapipe and a real GPU therefore report the same category
// totals for the same resource set, which is what makes a ratchet possible at all. The allocator's
// own `block_bytes` is NOT printed: that one is driver-specific and would make the row flaky.
//
// Usage: gpu_memory_probe [--backend vulkan|null]
// Exit codes: 0 measured, 2 usage error, 3 no device available (a skip, not a pass).

#include "tessera/rendering/null/null_device.hpp"
#if defined(TESSERA_PROBE_HAS_VULKAN)
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>

namespace {

using namespace tessera::rendering;

// The fixed resource set the budget row is recorded from. Changing it changes the row, which is the
// point: the number is a claim about this exact set, and a regression is a change in the accounting
// rather than a change in the driver.
constexpr std::uint32_t kAtlasPage = 1024; // R8 glyph atlas page: 1 MiB
constexpr std::uint32_t kImageWidth = 512; // RGBA decoded image: 1 MiB
constexpr std::uint32_t kImageHeight = 512;
constexpr std::size_t kVertexBytes = 64 * 1024;
constexpr std::size_t kStagingBytes = 32 * 1024;

void print_category(const char* name, const rhi::MemoryCategoryStats& stats) {
    std::printf("gpu_category_%s_bytes=%llu\n", name,
                static_cast<unsigned long long>(stats.live_bytes));
    std::printf("gpu_category_%s_peak_bytes=%llu\n", name,
                static_cast<unsigned long long>(stats.peak_bytes));
}

int measure(rhi::Device& device) {
    const auto base = device.memory_statistics();
    if (base.buffer_bytes != 0 || base.image_bytes != 0) {
        std::fprintf(stderr, "gpu_memory_probe: the device is not fresh\n");
        return 2;
    }

    rhi::TextureDesc atlas_desc;
    atlas_desc.extent = {kAtlasPage, kAtlasPage};
    atlas_desc.format = rhi::TextureFormat::R8Unorm;
    atlas_desc.category = rhi::MemoryCategory::GlyphAtlas;
    auto atlas = device.create_texture(atlas_desc);

    rhi::TextureDesc image_desc;
    image_desc.extent = {kImageWidth, kImageHeight};
    image_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    image_desc.category = rhi::MemoryCategory::Texture;
    auto image = device.create_texture(image_desc);

    auto vertices = device.create_buffer({kVertexBytes, rhi::BufferUsage::Vertex});
    auto staging = device.create_buffer({kStagingBytes, rhi::BufferUsage::Staging});

    const auto stats = device.memory_statistics();
    const auto& categories = stats.categories;

    print_category("glyph_atlas",
                   categories[static_cast<std::size_t>(rhi::MemoryCategory::GlyphAtlas)]);
    print_category("texture", categories[static_cast<std::size_t>(rhi::MemoryCategory::Texture)]);
    print_category("vertex", categories[static_cast<std::size_t>(rhi::MemoryCategory::Vertex)]);
    print_category("staging", categories[static_cast<std::size_t>(rhi::MemoryCategory::Staging)]);
    print_category("other", categories[static_cast<std::size_t>(rhi::MemoryCategory::Other)]);

    // The total is the row the budget actually ratchets: it is the sum of the categories, so a new
    // category that nobody budgets still moves it.
    std::uint64_t total = 0;
    std::uint64_t peak_total = 0;
    for (const auto& entry : categories) {
        total += entry.live_bytes;
        peak_total += entry.peak_bytes;
    }
    // The total is the row the budget actually ratchets: it is the sum of the categories, so a new
    // category that nobody budgets still moves it. The name is `gpu_vma_category_bytes` because
    // that is the row tests/golden/perf/memory_budget.json has declared since issue #13; keeping
    // the name means the entry that was `unmeasured` becomes measured rather than being replaced by
    // a second row nobody was watching.
    std::printf("gpu_vma_category_bytes=%llu\n", static_cast<unsigned long long>(total));
    std::printf("gpu_category_peak_total_bytes=%llu\n",
                static_cast<unsigned long long>(peak_total));

    // Release everything, then report the peak again: the peak must survive the free, which is the
    // property that makes it useful for a transient staging buffer.
    atlas.reset();
    image.reset();
    vertices.reset();
    staging.reset();
    const auto after = device.memory_statistics();
    std::uint64_t live_after = 0;
    std::uint64_t peak_after = 0;
    for (const auto& entry : after.categories) {
        live_after += entry.live_bytes;
        peak_after += entry.peak_bytes;
    }
    std::printf("gpu_category_live_after_free_bytes=%llu\n",
                static_cast<unsigned long long>(live_after));
    std::printf("gpu_category_peak_after_free_bytes=%llu\n",
                static_cast<unsigned long long>(peak_after));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const char* backend = "vulkan";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--backend") == 0) {
            backend = argv[i + 1];
        }
    }

    std::printf("# gpu_memory_probe: name=value rows for scripts/check_memory_budget.py\n");
    std::printf("# backend=%s\n", backend);

    if (std::strcmp(backend, "null") == 0) {
        auto device = null::create_null_device(rhi::DeviceDesc{});
        return measure(*device);
    }

#if defined(TESSERA_PROBE_HAS_VULKAN)
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = false;
    std::unique_ptr<rhi::Device> device;
    try {
        device = vulkan::create_vulkan_device(desc);
    } catch (const std::exception& error) {
        // No device is a skip, not a pass: the caller decides whether that is acceptable on its
        // lane. Exit 3 so a lane that expected a device can tell the difference.
        std::fprintf(stderr, "gpu_memory_probe: no Vulkan device (%s)\n", error.what());
        return 3;
    }
    return measure(*device);
#else
    std::fprintf(stderr, "gpu_memory_probe: built without the Vulkan backend\n");
    return 3;
#endif
}
