// SPDX-License-Identifier: MIT
//
// create_texture tail attribution of the 4K measurement tool (H1b): VMA block growth against image,
// view, sampler, descriptor and upload cost.

#include "cpu_to_gpu_4k_measure.hpp"

#include <memory>

namespace measure {

namespace {

[[nodiscard]] std::uint32_t blocks_of(rhi::Device& device) {
    return device.memory_statistics().block_count;
}

struct TimedCreate {
    double ms = 0.0;
    std::uint32_t blocks_before = 0;
    std::uint32_t blocks_after = 0;
};

[[nodiscard]] TimedCreate timed_create(rhi::Device& device, const rhi::TextureDesc& desc,
                                       std::unique_ptr<rhi::Texture>& out) {
    TimedCreate result;
    result.blocks_before = blocks_of(device);
    const auto start = Clock::now();
    out = device.create_texture(desc);
    result.ms = ms_since(start);
    result.blocks_after = blocks_of(device);
    return result;
}

void print_block_events(const std::string& name, const std::vector<TimedCreate>& calls) {
    std::vector<double> values;
    double with_alloc = 0.0;
    double without_alloc = 0.0;
    std::size_t with_count = 0;
    std::size_t without_count = 0;
    for (const TimedCreate& c : calls) {
        values.push_back(c.ms);
        if (c.blocks_after > c.blocks_before) {
            with_alloc += c.ms;
            ++with_count;
        } else {
            without_alloc += c.ms;
            ++without_count;
        }
    }
    print_tail(name, tail_of(values), calls.size());
    std::cout << "    calls that added a VMA block: " << with_count << " (avg " << std::fixed
              << std::setprecision(2) << (with_count != 0 ? with_alloc / with_count : 0.0)
              << " ms); calls that did not: " << without_count << " (avg "
              << (without_count != 0 ? without_alloc / without_count : 0.0) << " ms)\n";
}

} // namespace

// H1b: attribute the create_texture tail. Hypothesis: the tail is vkAllocateMemory for a new VMA
// block (memory freed and re-committed as textures churn), not image/view/sampler/descriptor
// creation and not the upload. Each experiment records VMA block_count around every call.
void measure_create_texture_tail(rhi::Device& device, const std::vector<std::uint8_t>& frame) {
    std::cout << "\n[H1b. What is in the create_texture tail? block_count read around each call]\n";
    rhi::TextureDesc with_pixels;
    with_pixels.extent = {k4kWidth, k4kHeight};
    with_pixels.pixels = frame.data();
    with_pixels.row_pitch = static_cast<std::size_t>(k4kWidth) * 4U;
    with_pixels.filter = rhi::Filter::Linear;
    rhi::TextureDesc no_pixels = with_pixels;
    no_pixels.pixels = nullptr;

    // A: one live texture at a time, so the allocator reuses the same block every call.
    std::vector<TimedCreate> steady;
    for (int i = 0; i < 60; ++i) {
        std::unique_ptr<rhi::Texture> texture;
        steady.push_back(timed_create(device, with_pixels, texture));
        device.wait_idle();
    }
    print_block_events("A. steady: 1 live, 4K with pixels", steady);

    // B: growth, every texture stays alive. New blocks must be allocated as memory fills.
    std::vector<std::unique_ptr<rhi::Texture>> live;
    std::vector<TimedCreate> growth;
    for (int i = 0; i < 12; ++i) {
        live.emplace_back();
        growth.push_back(timed_create(device, with_pixels, live.back()));
    }
    device.wait_idle();
    print_block_events("B. growth: 12 live 4K textures", growth);
    live.clear();
    device.wait_idle();

    // C: churn, 5 live then free all, repeated: is a freed block released and re-allocated?
    std::vector<TimedCreate> churn;
    for (int cycle = 0; cycle < 8; ++cycle) {
        std::vector<std::unique_ptr<rhi::Texture>> cycle_textures(5);
        for (auto& texture : cycle_textures) {
            churn.push_back(timed_create(device, with_pixels, texture));
        }
        device.wait_idle();
        cycle_textures.clear();
        device.wait_idle();
    }
    print_block_events("C. churn: 5 live, free all, x8 cycles", churn);
    std::cout << "    VMA blocks now: " << blocks_of(device) << "\n";

    // D: image creation alone (no staging, no copy) and a small image, to separate the stages.
    std::vector<TimedCreate> bare;
    for (int i = 0; i < 60; ++i) {
        std::unique_ptr<rhi::Texture> texture;
        bare.push_back(timed_create(device, no_pixels, texture));
        device.wait_idle();
    }
    print_block_events("D. 4K, NO pixels (image+view+sampler+set)", bare);
    rhi::TextureDesc small = with_pixels;
    small.extent = {64, 64};
    std::vector<std::uint8_t> small_pixels(64U * 64U * 4U, 0x11);
    small.pixels = small_pixels.data();
    small.row_pitch = 64U * 4U;
    std::vector<TimedCreate> tiny;
    for (int i = 0; i < 60; ++i) {
        std::unique_ptr<rhi::Texture> texture;
        tiny.push_back(timed_create(device, small, texture));
        device.wait_idle();
    }
    print_block_events("D. 64x64 with pixels", tiny);

    // E: destruction.
    std::vector<double> destroy;
    for (int i = 0; i < 40; ++i) {
        std::unique_ptr<rhi::Texture> texture;
        static_cast<void>(timed_create(device, with_pixels, texture));
        device.wait_idle();
        const auto start = Clock::now();
        texture.reset();
        destroy.push_back(ms_since(start));
    }
    print_tail("E. destroy 4K texture (after wait_idle)", tail_of(destroy), destroy.size());
}

} // namespace measure
