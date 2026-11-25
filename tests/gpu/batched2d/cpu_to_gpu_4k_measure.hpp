// SPDX-License-Identifier: MIT
//
// Shared helpers of the manual 4K CPU-to-GPU measurement tool (cpu_to_gpu_4k_measure.cpp): the
// clock, the min/median/max sample, the table printers and the per-call tail statistics.

#pragma once

#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/path/path_model.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace measure {

namespace path = tessera::rendering::path;
namespace image = tessera::rendering::image;
namespace rhi = tessera::rendering::rhi;

inline constexpr std::uint32_t k4kWidth = 3840;
inline constexpr std::uint32_t k4kHeight = 2160;
inline constexpr int kWarmup = 2;
inline constexpr int kIterations = 15;

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Sample {
    double min = 0.0;
    double median = 0.0;
    double max = 0.0;
};

[[nodiscard]] inline Sample summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values.front(), values[values.size() / 2], values.back()};
}

[[nodiscard]] inline Sample time_it(const std::function<void()>& body) {
    for (int i = 0; i < kWarmup; ++i) {
        body();
    }
    std::vector<double> values;
    for (int i = 0; i < kIterations; ++i) {
        const auto start = Clock::now();
        body();
        values.push_back(ms_since(start));
    }
    return summarize(std::move(values));
}

inline void print_row(const std::string& name, const Sample& sample, std::uint64_t bytes) {
    // bytes / ms / 1e6 = GB/s with decimal giga.
    const double gbps =
        sample.median > 0.0 ? static_cast<double>(bytes) / (sample.median * 1.0e6) : 0.0;
    std::cout << std::left << std::setw(46) << name << std::right << std::fixed
              << std::setprecision(3) << " min=" << std::setw(8) << sample.min
              << " med=" << std::setw(8) << sample.median << " max=" << std::setw(8) << sample.max
              << " ms  " << std::setprecision(2) << std::setw(6) << gbps << " GB/s\n";
}

inline void print_budget(const std::string& name, double median_ms) {
    std::cout << "  " << std::left << std::setw(44) << name << std::right << std::fixed
              << std::setprecision(1) << std::setw(7) << median_ms << " ms  = " << std::setw(5)
              << (median_ms / (1000.0 / 60.0)) * 100.0 << "% of 60Hz, " << std::setw(5)
              << (median_ms / (1000.0 / 120.0)) * 100.0 << "% of 120Hz, " << std::setw(5)
              << (median_ms / (1000.0 / 240.0)) * 100.0 << "% of 240Hz\n";
}

// Per-call latency distribution: the spike hypothesis is about the tail, so median alone hides it.
struct Tail {
    double p50 = 0.0;
    double p99 = 0.0;
    double max = 0.0;
    std::vector<std::size_t> spike_indices; // calls slower than 3x the median
};

[[nodiscard]] inline Tail tail_of(const std::vector<double>& values) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    Tail tail;
    tail.p50 = sorted[sorted.size() / 2];
    tail.p99 = sorted[std::min(sorted.size() - 1, sorted.size() * 99 / 100)];
    tail.max = sorted.back();
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] > 3.0 * tail.p50) {
            tail.spike_indices.push_back(i);
        }
    }
    return tail;
}

inline void print_tail(const std::string& name, const Tail& tail, std::size_t calls) {
    std::cout << std::left << std::setw(46) << name << std::right << std::fixed
              << std::setprecision(3) << " p50=" << std::setw(8) << tail.p50
              << " p99=" << std::setw(8) << tail.p99 << " max=" << std::setw(8) << tail.max
              << " ms  spikes(>3x p50)=" << tail.spike_indices.size() << "/" << calls;
    if (!tail.spike_indices.empty()) {
        std::cout << " at call#";
        for (std::size_t i = 0; i < std::min<std::size_t>(tail.spike_indices.size(), 8); ++i) {
            std::cout << ' ' << tail.spike_indices[i];
        }
    }
    std::cout << "\n";
}

// The same arithmetic as the image cache's `resample_nearest`.
[[nodiscard]] std::vector<std::uint8_t> resample_like_cache(const std::vector<std::uint8_t>& src,
                                                            std::uint32_t src_w,
                                                            std::uint32_t src_h, std::uint32_t w,
                                                            std::uint32_t h);

void measure_staging_hypothesis(rhi::Device& device, const std::vector<std::uint8_t>& frame);

// H6: the image path's decode-target downscale in isolation (issue #689 work item 1).
//
// `downscale_box` (`src/rendering/image/image_resize.cpp`) is a per-output-pixel CPU loop with four
// 64-bit sums and four integer divides, run on the decode path whenever a decode target is smaller
// than the source. #682 measured its sibling `resample_nearest` at 0.55 GB/s; this loop does
// strictly more per output pixel, but it had never been measured on its own, which is why the
// decision to move it to the GPU was waiting.
//
// It takes no device on purpose: the point is the CPU cost of the function the image path calls, so
// the number is available on a machine with no GPU and on the lavapipe lane, neither of which can
// run the rest of this tool.
void measure_downscale_box(const std::vector<std::uint8_t>& source_4k);
void measure_resample_hypothesis(const std::vector<std::uint8_t>& frame);
void measure_encode_hypothesis();
void measure_nearest_exactness(rhi::Device& device);
void measure_create_texture_tail(rhi::Device& device, const std::vector<std::uint8_t>& frame);

} // namespace measure
