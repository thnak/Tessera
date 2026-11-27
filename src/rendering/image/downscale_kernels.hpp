// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_loader.hpp"

#include <cstddef>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#define TESSERA_DOWNSCALE_X86 1
#else
#define TESSERA_DOWNSCALE_X86 0
#endif

namespace tessera::rendering::image {

// Instruction sets `downscale_box` can run its inner loop on. Every one produces the same bytes
// (integer arithmetic only), so the choice is speed, never output; `portable` is the reference.
enum class DownscaleIsa : std::uint8_t { portable, sse2, avx2, avx512 };

[[nodiscard]] const char* isa_name(DownscaleIsa isa) noexcept;

// True when this CPU and OS can run `isa` (`portable` always; x86 sets are probed with CPUID and
// XGETBV, so a CPU whose OS has not enabled the wide registers is reported as unsupported).
[[nodiscard]] bool isa_supported(DownscaleIsa isa) noexcept;

// The widest supported set, probed once.
[[nodiscard]] DownscaleIsa best_isa() noexcept;

// `downscale_box` on a chosen instruction set; an unsupported `isa` runs `portable`. Exists so a
// test can hold every kernel to the same bytes and a benchmark can compare them.
[[nodiscard]] DecodedImage downscale_box_with(DownscaleIsa isa, const DecodedImage& source,
                                              std::uint32_t width, std::uint32_t height);

// A kernel adds one source row into four planar u32 accumulators (red*alpha, green*alpha,
// blue*alpha, alpha), each `stride` entries apart: `planes[c * stride + x]` for pixel x. Values
// are at most 255 * 255, so a plane entry stays below 2^32 for up to 65536 accumulated texels.
using AccumulateRowFn = void (*)(const std::uint8_t* rgba_row, std::uint32_t* planes,
                                 std::size_t stride, std::size_t pixels);

// The scalar loop over pixels [first, pixels), which is also every SIMD kernel's tail.
void accumulate_row_scalar(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                           std::size_t first, std::size_t pixels) noexcept;

#if TESSERA_DOWNSCALE_X86
[[nodiscard]] bool x86_avx2_available() noexcept;
[[nodiscard]] bool x86_avx512_available() noexcept;
void accumulate_row_sse2(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                         std::size_t pixels);
void accumulate_row_avx2(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                         std::size_t pixels);
void accumulate_row_avx512(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                           std::size_t pixels);
#endif

} // namespace tessera::rendering::image
