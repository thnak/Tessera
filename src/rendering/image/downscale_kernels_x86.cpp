// SPDX-License-Identifier: MIT

#include "downscale_kernels.hpp"

#if TESSERA_DOWNSCALE_X86

#include <immintrin.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// GCC and Clang need the target on the function to use the intrinsics without a global -mavx2;
// MSVC compiles them as they are. Dispatch (downscale_kernels.cpp) only calls a kernel the CPU has.
#if defined(__GNUC__) || defined(__clang__)
#define TESSERA_TARGET(features) __attribute__((target(features)))
#else
#define TESSERA_TARGET(features)
#endif

namespace tessera::rendering::image {

namespace {

struct Features {
    bool avx2 = false;
    bool avx512 = false;
};

Features probe() noexcept {
    Features f;
#if defined(_MSC_VER)
    int info[4] = {};
    __cpuid(info, 0);
    if (info[0] < 7) {
        return f;
    }
    __cpuid(info, 1);
    const bool os_xsave = ((info[2] >> 27) & 1) != 0;
    const bool avx = ((info[2] >> 28) & 1) != 0;
    if (!os_xsave || !avx) {
        return f;
    }
    // XCR0: bits 1-2 are the XMM/YMM state, bits 5-7 the opmask and ZMM state; without them the OS
    // does not preserve the registers across a context switch and the CPU flag alone is not enough.
    const unsigned long long xcr0 = _xgetbv(0);
    const bool ymm = (xcr0 & 0x6U) == 0x6U;
    const bool zmm = (xcr0 & 0xE0U) == 0xE0U;
    __cpuidex(info, 7, 0);
    f.avx2 = ymm && ((info[1] >> 5) & 1) != 0;
    f.avx512 = f.avx2 && zmm && ((info[1] >> 16) & 1) != 0 && ((info[1] >> 30) & 1) != 0;
#else
    __builtin_cpu_init();
    f.avx2 = __builtin_cpu_supports("avx2") != 0;
    f.avx512 = __builtin_cpu_supports("avx512f") != 0 && __builtin_cpu_supports("avx512bw") != 0;
#endif
    return f;
}

const Features& features() noexcept {
    static const Features cached = probe();
    return cached;
}

} // namespace

bool x86_avx2_available() noexcept {
    return features().avx2;
}

bool x86_avx512_available() noexcept {
    return features().avx512;
}

// Each pixel is a 32-bit lane. A channel is at most 255, so the product of two of them is at most
// 65025 and a 16-bit multiply of lanes whose upper half is zero is exact, with a zero upper half
// again: mullo_epi16 is the cheapest exact multiply here.

void accumulate_row_sse2(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                         std::size_t pixels) {
    std::uint32_t* const pr = planes;
    std::uint32_t* const pg = planes + stride;
    std::uint32_t* const pb = planes + 2 * stride;
    std::uint32_t* const pa = planes + 3 * stride;
    const __m128i mask = _mm_set1_epi32(0xFF);
    std::size_t i = 0;
    for (; i + 4 <= pixels; i += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba_row + i * 4));
        const __m128i a = _mm_srli_epi32(v, 24);
        const __m128i r = _mm_and_si128(v, mask);
        const __m128i g = _mm_and_si128(_mm_srli_epi32(v, 8), mask);
        const __m128i b = _mm_and_si128(_mm_srli_epi32(v, 16), mask);
        auto add = [i](std::uint32_t* plane, __m128i value) {
            auto* p = reinterpret_cast<__m128i*>(plane + i);
            _mm_storeu_si128(p, _mm_add_epi32(_mm_loadu_si128(p), value));
        };
        add(pr, _mm_mullo_epi16(r, a));
        add(pg, _mm_mullo_epi16(g, a));
        add(pb, _mm_mullo_epi16(b, a));
        add(pa, a);
    }
    accumulate_row_scalar(rgba_row, planes, stride, i, pixels);
}

TESSERA_TARGET("avx2")
void accumulate_row_avx2(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                         std::size_t pixels) {
    std::uint32_t* const pr = planes;
    std::uint32_t* const pg = planes + stride;
    std::uint32_t* const pb = planes + 2 * stride;
    std::uint32_t* const pa = planes + 3 * stride;
    const __m256i mask = _mm256_set1_epi32(0xFF);
    std::size_t i = 0;
    for (; i + 8 <= pixels; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rgba_row + i * 4));
        const __m256i a = _mm256_srli_epi32(v, 24);
        const __m256i r = _mm256_and_si256(v, mask);
        const __m256i g = _mm256_and_si256(_mm256_srli_epi32(v, 8), mask);
        const __m256i b = _mm256_and_si256(_mm256_srli_epi32(v, 16), mask);
        auto add = [i](std::uint32_t* plane, __m256i value) {
            auto* p = reinterpret_cast<__m256i*>(plane + i);
            _mm256_storeu_si256(p, _mm256_add_epi32(_mm256_loadu_si256(p), value));
        };
        add(pr, _mm256_mullo_epi16(r, a));
        add(pg, _mm256_mullo_epi16(g, a));
        add(pb, _mm256_mullo_epi16(b, a));
        add(pa, a);
    }
    accumulate_row_scalar(rgba_row, planes, stride, i, pixels);
}

TESSERA_TARGET("avx512f,avx512bw")
void accumulate_row_avx512(const std::uint8_t* rgba_row, std::uint32_t* planes, std::size_t stride,
                           std::size_t pixels) {
    std::uint32_t* const pr = planes;
    std::uint32_t* const pg = planes + stride;
    std::uint32_t* const pb = planes + 2 * stride;
    std::uint32_t* const pa = planes + 3 * stride;
    const __m512i mask = _mm512_set1_epi32(0xFF);
    std::size_t i = 0;
    for (; i + 16 <= pixels; i += 16) {
        const __m512i v = _mm512_loadu_si512(rgba_row + i * 4);
        const __m512i a = _mm512_srli_epi32(v, 24);
        const __m512i r = _mm512_and_si512(v, mask);
        const __m512i g = _mm512_and_si512(_mm512_srli_epi32(v, 8), mask);
        const __m512i b = _mm512_and_si512(_mm512_srli_epi32(v, 16), mask);
        auto add = [i](std::uint32_t* plane, __m512i value) {
            _mm512_storeu_si512(plane + i, _mm512_add_epi32(_mm512_loadu_si512(plane + i), value));
        };
        add(pr, _mm512_mullo_epi16(r, a));
        add(pg, _mm512_mullo_epi16(g, a));
        add(pb, _mm512_mullo_epi16(b, a));
        add(pa, a);
    }
    accumulate_row_scalar(rgba_row, planes, stride, i, pixels);
}

} // namespace tessera::rendering::image

#endif // TESSERA_DOWNSCALE_X86
