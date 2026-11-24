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

// `Metal` is the native macOS/iOS backend (ADR-053), not MoltenVK: it is a distinct API, so it is a
// distinct enumerator. A build that does not compile it in never reports it.
enum class Backend : std::uint8_t { Null, Vulkan, WebGpu, Metal };

// Color-renderable 8-bit formats. The `*Srgb` variants have the same byte layout as their UNORM
// siblings but store/return sRGB-encoded values: attachment writes are encoded from linear light
// and texture samples are decoded to linear. Offscreen golden/deterministic targets stay UNORM
// unless a caller asks for sRGB, and so does presentation: the paint pipeline's output is already
// sRGB-encoded, so an sRGB presentation surface would encode it twice (issue #791). The sRGB
// formats are for a path that really holds linear light or authored sRGB bytes needing a decode on
// sample.
enum class TextureFormat : std::uint8_t {
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

// What a buffer is for. `Storage` (RHI 1.12, ADR-068) is a read-only shader buffer of arbitrary
// size, bound by `CommandList::bind_storage_buffer`; it is the seam that lets a variable-length
// record list (clip shapes, ADR-068) reach the fragment shader without a fixed push-constant slot.
// It is appended last so no existing enumerator changes value.
enum class BufferUsage : std::uint8_t { Vertex, Index, Uniform, Staging, Readback, Storage };

// The memory category a resource is charged to (RHI 1.14, issue #703). memory-model-v1 section 4
// asks for explicit per-category GPU budgets, and a budget needs a category to attach to: the
// allocator's totals say how much the device holds, not what it is holding it *for*, so a texture
// regression and a vertex-buffer regression look identical in `DeviceMemoryStats::image_bytes`.
//
// The split is by *purpose*, not by resource kind, because that is what a budget can act on: a
// glyph atlas page and a decoded image are both textures, but only one of them is bounded by the
// atlas page limit. `Other` is the honest default for a resource whose purpose the RHI cannot know
// (a caller that creates a texture directly), so an untagged resource is visible rather than
// silently attributed to a category it may not belong to.
//
// Appended as a new enum rather than folded into `BufferUsage`: a texture has no `BufferUsage`, and
// the categories span both resource kinds.
enum class MemoryCategory : std::uint8_t {
    Vertex,
    Index,
    Uniform,
    Staging,
    Readback,
    Storage,
    Texture,
    RenderTarget,
    GlyphAtlas,
    PathCache,
    Other,
};

// Number of `MemoryCategory` values, for a fixed-size per-category array. Kept beside the enum so
// adding a category without widening the arrays is a compile error rather than a silent truncation.
inline constexpr std::size_t kMemoryCategoryCount = 11;

// Per-category live bytes and the high-water mark since the device was created (RHI 1.14, issue
// #703). `peak_bytes` is what makes a transient spike visible: a staging buffer that exists for one
// upload is gone by the time anyone samples, so the live figure alone understates what the device
// had to find room for.
struct MemoryCategoryStats {
    std::uint64_t live_bytes = 0;
    std::uint64_t peak_bytes = 0;
};

enum class Filter : std::uint8_t { Nearest, Linear };

enum class AddressMode : std::uint8_t { ClampToEdge, Repeat };

// How a pipeline's fragment output is combined with the destination.
//
// `DualSource` is per-channel coverage (RHI 1.5, issue #471): the fragment shader writes colour to
// output 0 and coverage to output 1, and the blend is `src0 * src1 + dst * (1 - src1)` per channel.
// It exists for LCD subpixel text, where the three coverage channels differ per pixel and a single
// alpha cannot express them. It is a capability, not floor: a device without dual-source blending
// reports false from `Device::supports_dual_source_blend()` and a pipeline request for it must fail
// at creation, because the documented fallback is grayscale coverage (one alpha), not a silently
// different blend. `None` and `Alpha` are unchanged.
enum class BlendMode : std::uint8_t { None, Alpha, DualSource };

enum class LoadOp : std::uint8_t { Clear, Load, DontCare };

enum class StoreOp : std::uint8_t { Store, DontCare };

// Vertex attribute formats. `UInt` is a single unsigned integer (RHI 1.7, issue #480 part 2): the
// per-vertex texture-array index the batched 2D shader reads to pick its sampled image.
enum class VertexFormat : std::uint8_t { Float, Float2, Float3, Float4, UByte4Norm, UInt };

// The window-system family a presentation surface belongs to (RHI 1.4, issue #155). `Auto` lets the
// backend resolve it at instance/device creation from the platform build and the process
// environment (`WAYLAND_DISPLAY`, `DISPLAY`), which is what keeps every existing caller working
// unchanged. A caller that knows which HAL window it will present to passes the matching value.
// The "no presentation" enumerator is `NoPlatform`, not `None`: X11's X.h defines `None` as a macro
// and this public header must stay includable from translation units that also include Xlib.
enum class SurfacePlatform : std::uint8_t {
    Auto = 0,
    NoPlatform,
    Win32,
    Xlib,
    Wayland,
    // Android: the HAL window's `native_handle().window` is an `ANativeWindow*`, and the surface is
    // created with `vkCreateAndroidSurfaceKHR` (VK_KHR_android_surface). There is no
    // `vkGetPhysicalDevice*PresentationSupportKHR` for this family: a queue family that supports
    // graphics on an Android device is assumed presentable, which is what the platform guarantees.
    Android,
};

struct DeviceDesc {
    Backend backend = Backend::Vulkan;
    bool validation = false;
    int device_index = -1;
    // The presentation surface family this device will be asked to create swapchains for. The
    // backend may enable more than one surface extension (a Linux box can build both Xlib and
    // Wayland support, and enabling an unused instance extension is harmless), but only the
    // resolved family is probed for queue presentation support and accepted by
    // `create_swapchain`; see docs/design/gpu/rhi.md section 4.
    SurfacePlatform surface = SurfacePlatform::Auto;
};

// A buffer request. `size` must be non-zero: Vulkan cannot create a zero-size buffer at all
// (`vkCreateBuffer` reports VK_ERROR_INITIALIZATION_FAILED), so a zero-size request is a caller bug
// every backend rejects with std::invalid_argument instead of each failing differently. A caller
// that has nothing to upload skips the creation rather than asking for an empty buffer.
struct BufferDesc {
    std::size_t size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    // RHI 1.16 (issue #683): a `Storage` buffer that shaders write as well as read, kept in device
    // memory. It is not host-mappable (`Buffer::write`/`read` throw std::runtime_error): fill it
    // from a compute pass and read it back with `CommandList::copy_buffer` into a `Readback`
    // buffer. Ignored for every other usage. A device without compute ignores it too, and the
    // buffer is then the plain read-only storage buffer.
    bool gpu_read_write = false;
};

// A compute pipeline (RHI 1.16, issue #683). SPIR-V is the only source form, like `PipelineDesc`;
// the entry point is `main`. The pipeline owns one descriptor set of `storage_buffer_count`
// read/write storage buffers at bindings 0..count-1, bound with `bind_compute_buffer`.
struct ComputePipelineDesc {
    const std::uint32_t* spirv = nullptr;
    std::size_t spirv_size = 0; // bytes
    std::uint32_t storage_buffer_count = 0; // 1..kMaxComputeStorageBuffers
    std::uint32_t push_constant_size = 0;   // bytes, a multiple of 4
};

inline constexpr std::uint32_t kMaxComputeStorageBuffers = 8;

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
    // Mip levels (RHI 1.6, issue #491): 1 (default) is a single level. A value above 1 asks the
    // device to build the chain from the uploaded base level (`pixels`, required) with a box-style
    // downsample, and the sampler then selects a level from the draw's minification. At most
    // `full_mip_levels(extent)`, only for sample_count == 1, and only when
    // `Device::supports_texture_mipmaps(format)` is true; otherwise `create_texture` throws. A
    // mipmapped texture is immutable: `Texture::write` throws, because the chain would go stale.
    std::uint32_t levels = 1;
    // Which budget this texture is charged to (RHI 1.14, issue #703). The default is `Other`, so a
    // caller that does not care is counted and visible rather than misattributed. A cache that owns
    // a bounded pool (the glyph atlas, the path cache) sets its own category so its budget row can
    // be checked against the device's own accounting instead of a second, drifting tally.
    MemoryCategory category = MemoryCategory::Other;
};

// The length of a complete mip chain for a base extent: floor(log2(max(width, height))) + 1.
// A zero extent has one level.
[[nodiscard]] constexpr std::uint32_t full_mip_levels(Extent2D extent) noexcept {
    std::uint32_t largest = extent.width > extent.height ? extent.width : extent.height;
    std::uint32_t levels = 1;
    while (largest > 1U) {
        largest >>= 1U;
        ++levels;
    }
    return levels;
}

// Whether a texture request's level count is well formed (RHI 1.6): in [1, full chain], and more
// than one level only for a 1-sample texture that uploads its base level. Backends throw
// std::invalid_argument when this is false, before they look at device support.
[[nodiscard]] constexpr bool valid_texture_levels(const TextureDesc& desc) noexcept {
    if (desc.levels < 1U || desc.levels > full_mip_levels(desc.extent)) {
        return false;
    }
    return desc.levels == 1U || (desc.sample_count == 1U && desc.pixels != nullptr);
}

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
    // Sampler state the target is read with when `CommandList::bind_sampled_target` binds it as a
    // texture (RHI 1.3). It is fixed at creation - like a texture's filter/address - so a bind
    // allocates nothing on the frame path. The defaults are the state the glass blur needs: linear
    // filtering (the separable Gaussian pairs adjacent texels into one bilinear tap) and
    // clamp-to-edge addressing (so a glass node at a window border does not darken). The two fields
    // describe sampling only; `begin_render_pass` ignores them.
    Filter filter = Filter::Linear;
    AddressMode address = AddressMode::ClampToEdge;
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
    // True when the shader indexes a sampled-image array bound by `CommandList::bind_texture_array`
    // (RHI 1.7, issue #480 part 2), so the pipeline layout must declare that array. A backend
    // without the capability (`Device::texture_array_capacity() == 0`) throws instead of building a
    // pipeline whose descriptor set can never be bound. False keeps the one-set-per-texture layout.
    bool texture_array = false;
    // True when the shader reads a read-only storage buffer bound by
    // `CommandList::bind_storage_buffer` (RHI 1.12, ADR-068), so the pipeline layout must declare
    // that binding. A backend without the capability (`Device::supports_storage_buffers()` false)
    // throws instead of building a pipeline whose descriptor set can never be bound. False keeps
    // the push-constant-only layout every existing pipeline uses.
    bool storage_buffer = false;
};

// Supported MSAA sample counts for the 1-sample-first RHI. Values outside this set are rejected at
// resource creation with an explicit error (never silently lowered).
inline constexpr std::uint32_t kSampleCount1 = 1;
inline constexpr std::uint32_t kSampleCount4 = 4;

[[nodiscard]] constexpr bool is_supported_sample_count(std::uint32_t sample_count) {
    return sample_count == kSampleCount1 || sample_count == kSampleCount4;
}

} // namespace tessera::rendering::rhi
