// SPDX-License-Identifier: MIT

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::rendering::rhi {

struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct Offset2D {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

struct Rect {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;

    [[nodiscard]] float right() const {
        return x + width;
    }
    [[nodiscard]] float bottom() const {
        return y + height;
    }

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && py >= y && px < right() && py < bottom();
    }
};

struct Color {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
};

constexpr Color rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255) {
    constexpr float kScale = 1.0f / 255.0f;
    return Color{static_cast<float>(r) * kScale, static_cast<float>(g) * kScale,
                 static_cast<float>(b) * kScale, static_cast<float>(a) * kScale};
}

struct Scissor {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline Scissor intersect(const Scissor& a, const Scissor& b) {
    const std::int32_t left = a.x > b.x ? a.x : b.x;
    const std::int32_t top = a.y > b.y ? a.y : b.y;
    const std::int32_t right =
        (a.x + static_cast<std::int32_t>(a.width)) < (b.x + static_cast<std::int32_t>(b.width))
            ? (a.x + static_cast<std::int32_t>(a.width))
            : (b.x + static_cast<std::int32_t>(b.width));
    const std::int32_t bottom =
        (a.y + static_cast<std::int32_t>(a.height)) < (b.y + static_cast<std::int32_t>(b.height))
            ? (a.y + static_cast<std::int32_t>(a.height))
            : (b.y + static_cast<std::int32_t>(b.height));
    if (right <= left || bottom <= top) {
        return Scissor{};
    }
    return Scissor{left, top, static_cast<std::uint32_t>(right - left),
                   static_cast<std::uint32_t>(bottom - top)};
}

struct Viewport {
    float x = 0;
    float y = 0;
    float width = 0;
    float height = 0;
    float min_depth = 0;
    float max_depth = 1;
};

// Row-major affine transform:
//   [ m0 m1 m2 ]   [ x ]
//   [ m3 m4 m5 ] * [ y ]
//   [  0  0  1 ]   [ 1 ]
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    [[nodiscard]] static Mat3 identity() {
        return Mat3{};
    }

    [[nodiscard]] static Mat3 translation(float x, float y) {
        Mat3 result;
        result.m[2] = x;
        result.m[5] = y;
        return result;
    }

    [[nodiscard]] static Mat3 scale(float x, float y) {
        Mat3 result;
        result.m[0] = x;
        result.m[4] = y;
        return result;
    }

    [[nodiscard]] static Mat3 rotation(float radians);

    [[nodiscard]] Mat3 operator*(const Mat3& other) const;

    void transform_point(float x, float y, float& out_x, float& out_y) const {
        out_x = m[0] * x + m[1] * y + m[2];
        out_y = m[3] * x + m[4] * y + m[5];
    }

    [[nodiscard]] bool try_inverse(Mat3& out) const;
};

inline Mat3 Mat3::rotation(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat3 result;
    result.m[0] = c;
    result.m[1] = -s;
    result.m[3] = s;
    result.m[4] = c;
    return result;
}

inline Mat3 Mat3::operator*(const Mat3& other) const {
    Mat3 result;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            float sum = 0;
            for (int k = 0; k < 3; ++k) {
                sum += m[row * 3 + k] * other.m[k * 3 + col];
            }
            result.m[row * 3 + col] = sum;
        }
    }
    return result;
}

inline bool Mat3::try_inverse(Mat3& out) const {
    const float determinant = m[0] * m[4] - m[1] * m[3];
    if (std::abs(determinant) < 1.0e-6f) {
        return false;
    }

    const float inverse_determinant = 1.0f / determinant;
    out = Mat3{};
    out.m[0] = m[4] * inverse_determinant;
    out.m[1] = -m[1] * inverse_determinant;
    out.m[2] = (m[1] * m[5] - m[4] * m[2]) * inverse_determinant;
    out.m[3] = -m[3] * inverse_determinant;
    out.m[4] = m[0] * inverse_determinant;
    out.m[5] = (m[3] * m[2] - m[0] * m[5]) * inverse_determinant;
    return true;
}

enum class Backend { Null, Vulkan, WebGpu };

// Color-renderable 8-bit formats. The `*Srgb` variants have the same byte layout as their UNORM
// siblings but store/return sRGB-encoded values: attachment writes are encoded from linear light
// and texture samples are decoded to linear. Production presentation prefers an sRGB surface format
// (D2); offscreen golden/deterministic targets stay UNORM unless a caller asks for sRGB.
enum class TextureFormat {
    R8G8B8A8Unorm,
    R8Unorm,
    B8G8R8A8Unorm,
    R8G8B8A8Srgb,
    B8G8R8A8Srgb,
};

// True for the sRGB-encoded variants (same layout as their UNORM sibling, different transfer
// function).
[[nodiscard]] constexpr bool is_srgb(TextureFormat format) {
    return format == TextureFormat::R8G8B8A8Srgb || format == TextureFormat::B8G8R8A8Srgb;
}

enum class BufferUsage { Vertex, Index, Uniform, Staging, Readback };

enum class Filter { Nearest, Linear };

enum class AddressMode { ClampToEdge, Repeat };

enum class BlendMode { None, Alpha };

enum class LoadOp { Clear, Load, DontCare };

enum class StoreOp { Store, DontCare };

enum class VertexFormat { Float, Float2, Float3, Float4, UByte4Norm };

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    bool validation = false;
    int device_index = -1;
};

struct BufferDesc {
    std::size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
};

struct TextureDesc {
    Extent2D extent;
    TextureFormat format = TextureFormat::R8G8B8A8Unorm;
    const void* pixels = nullptr;
    std::size_t row_pitch = 0;
    Filter filter = Filter::Nearest;
    AddressMode address = AddressMode::ClampToEdge;
    // MSAA sample count: 1 (default) or 4. Multisampled textures are storage/render-target style
    // resources; the 2D pipeline samples 1-sample textures only and pixels cannot be uploaded for
    // sample_count > 1. Backends report support through Device::supports_sample_count.
    std::uint32_t sample_count = 1;
};

struct RenderTargetDesc {
    Extent2D extent;
    TextureFormat format = TextureFormat::R8G8B8A8Unorm;
    Color clear_color{0.02f, 0.02f, 0.03f, 1.0f};
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::Store;
    // MSAA sample count: 1 (default) or 4. A 4x target renders into a multisample image and is
    // resolved into an internal 1-sample image at the end of the render pass; readback and
    // sampling see the resolved image.
    std::uint32_t sample_count = 1;
};

struct VertexAttribute {
    std::uint32_t location = 0;
    std::uint32_t offset = 0;
    VertexFormat format = VertexFormat::Float;
};

struct VertexBufferLayout {
    std::uint32_t stride = 0;
    const VertexAttribute* attributes = nullptr;
    std::uint32_t attribute_count = 0;
};

struct PipelineDesc {
    const std::uint32_t* vertex_spirv = nullptr;
    std::size_t vertex_spirv_size = 0;
    const std::uint32_t* fragment_spirv = nullptr;
    std::size_t fragment_spirv_size = 0;
    VertexBufferLayout vertex_layout;
    TextureFormat color_format = TextureFormat::R8G8B8A8Unorm;
    BlendMode blend = BlendMode::Alpha;
    std::uint32_t push_constant_size = 0;
    // MSAA sample count; must match the render target's sample count at draw time (1 or 4).
    std::uint32_t sample_count = 1;
    // Stable logical shader name from the shader manifest (for example "batched2d"; ADR-049).
    // Backends that do not consume SPIR-V resolve it against a shader table built at compile time.
    // Empty means "use the SPIR-V words above"; Vulkan and null ignore it. The view must point at
    // storage that outlives pipeline creation (a string literal or a constant in practice).
    std::string_view shader_id;
};

// Supported MSAA sample counts for the 1-sample-first RHI. Values outside this set are rejected at
// resource creation with an explicit error (never silently lowered).
inline constexpr std::uint32_t kSampleCount1 = 1;
inline constexpr std::uint32_t kSampleCount4 = 4;

[[nodiscard]] constexpr bool is_supported_sample_count(std::uint32_t sample_count) {
    return sample_count == kSampleCount1 || sample_count == kSampleCount4;
}

} // namespace tessera::rendering::rhi
