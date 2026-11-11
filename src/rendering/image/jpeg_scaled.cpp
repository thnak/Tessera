// SPDX-License-Identifier: MIT

#include "jpeg_scaled.hpp"

#include <csetjmp>
#include <cstddef>
#include <cstdio>

extern "C" {
#include <jerror.h>
#include <jpeglib.h>
}

namespace tessera::rendering::image {
namespace {

// std::jmp_buf needs 16-byte alignment on MSVC, so the struct is padded past `base`. The tail
// padding is expected and harmless; suppress only that warning (C4324) for this definition.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct ErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// libjpeg reports fatal errors by calling error_exit, which must not return: unwind to the setjmp
// in decode_jpeg_scaled. Nothing with a destructor lives between the two (only C frames).
void on_error(j_common_ptr cinfo) {
    auto* manager = reinterpret_cast<ErrorManager*>(cinfo->err);
    std::longjmp(manager->jump, 1);
}

void on_message(j_common_ptr) {}

} // namespace

unsigned jpeg_scale_denominator(std::uint32_t width, std::uint32_t height,
                                std::uint32_t target_width, std::uint32_t target_height) noexcept {
    unsigned best = 1;
    for (const unsigned denominator : {2U, 4U, 8U}) {
        const std::uint32_t scaled_width = (width + denominator - 1U) / denominator;
        const std::uint32_t scaled_height = (height + denominator - 1U) / denominator;
        if (scaled_width < target_width || scaled_height < target_height) {
            break;
        }
        best = denominator;
    }
    return best;
}

// setjmp/longjmp is how libjpeg reports fatal errors. Only C frames sit between the setjmp and the
// longjmp (see on_error), so the C4611 "non-portable with C++ destruction" warning does not apply.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4611)
#endif
bool decode_jpeg_scaled(std::span<const std::uint8_t> bytes, unsigned denominator,
                        JpegRaster& out) {
    if (bytes.empty() ||
        (denominator != 1U && denominator != 2U && denominator != 4U && denominator != 8U)) {
        return false;
    }
    jpeg_decompress_struct cinfo{};
    ErrorManager errors{};
    cinfo.err = jpeg_std_error(&errors.base);
    errors.base.error_exit = on_error;
    errors.base.output_message = on_message;

    if (setjmp(errors.jump) != 0) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), static_cast<unsigned long>(bytes.size()));
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK || cinfo.data_precision != 8 ||
        (cinfo.jpeg_color_space != JCS_YCbCr && cinfo.jpeg_color_space != JCS_GRAYSCALE &&
         cinfo.jpeg_color_space != JCS_RGB)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denominator;
    // The integer IDCT and plain upsampling are exact functions of the input, so the raster is
    // identical on every platform (SIMD is not built).
    cinfo.dct_method = JDCT_ISLOW;
    cinfo.do_fancy_upsampling = TRUE;
    jpeg_start_decompress(&cinfo);

    const std::uint32_t width = cinfo.output_width;
    const std::uint32_t height = cinfo.output_height;
    if (width == 0U || height == 0U || cinfo.output_components != 4) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    out.width = width;
    out.height = height;
    out.rgba.assign(static_cast<std::size_t>(width) * height * 4U, 0U);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row =
            out.rgba.data() + static_cast<std::size_t>(cinfo.output_scanline) * width * 4U;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace tessera::rendering::image
