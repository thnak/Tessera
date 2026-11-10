// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shader manifest loading and validation (docs/specs/shader-manifest-v1.md).
//
// The manifest describes one logical shader per entry with its stages, entry points, Vulkan/WebGPU
// sources, vertex layout, bindings, push constants and tests, so the two backend variants cannot
// silently diverge. This module owns the section 1-3 file shape, the section 4 compatibility
// rules, and the section 9 validation limits: strict key sets, required fields, type checks,
// bounds, and duplicate rejection.
//
// Recoverable failures are values, never exceptions: every entry point returns
// `std::expected<T, ValidationError>` with a stable error code and a stable JSON field path
// (`$.shaders[0].stages[1]`). Validation runs at build time; nothing here touches the UI or GPU.
//
// Decoder choice: this module decodes JSON itself rather than depending on `tessera_ir` or
// `tessera_harness`. `tessera_schema` is a leaf library by contract (machine-readable.md section 4
// and the LINK_LIBRARIES guard in src/schema/CMakeLists.txt), the IR's `JsonValue` has no decoder,
// and linking the harness codec would invert the dependency. The schema-local reader is strictly
// internal and is not part of this public API.
//
// Error codes are append-only: existing codes keep their meaning for diagnostics and build logs.
namespace tessera::schema {

// Version accepted by this module (section 6). A different integer is `UnsupportedVersion`.
inline constexpr std::uint32_t kManifestVersion = 1;

// Largest manifest accepted, in bytes (section 9). Manifests are small, generated build inputs;
// the cap bounds memory for untrusted files.
inline constexpr std::size_t kMaxManifestBytes = 1024 * 1024;

// Largest JSON nesting depth accepted (section 9), counting objects and arrays.
inline constexpr std::size_t kMaxManifestDepth = 32;

// Section 3.1. `Compute` is a reserved stage token: the validator accepts the token only to reject
// it with `ComputeStageUnsupported`, so it never appears in a returned `Manifest`.
enum class ShaderKind : std::uint8_t {
    Element,
    ViewTransitionEffect,
};

enum class ShaderStage : std::uint8_t {
    Vertex,
    Fragment,
    Compute,
};

enum class ShaderBindingType : std::uint8_t {
    UniformBuffer,
    StorageBuffer,
    Sampler,
    SampledTexture,
    StorageTexture,
};

// Section 3: one declared stage and its entry point (default `main`).
struct StageEntry {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;

    friend bool operator==(const StageEntry&, const StageEntry&) = default;
};

// Section 3 `vertexLayout`: explicit location/format/semantic, never implicit packing.
struct VertexAttribute {
    std::uint32_t location = 0;
    std::string format;
    std::string semantic;

    friend bool operator==(const VertexAttribute&, const VertexAttribute&) = default;
};

// Section 3 `bindings`: group/binding/type/name, expressible as a descriptor set and a bind group.
struct ShaderBinding {
    std::uint32_t group = 0;
    std::uint32_t binding = 0;
    ShaderBindingType type = ShaderBindingType::UniformBuffer;
    std::string name;

    friend bool operator==(const ShaderBinding&, const ShaderBinding&) = default;
};

// Section 3 `pushConstants.fields`: a 4-byte-aligned slot inside `PushConstants::size`.
struct PushConstantField {
    std::string name;
    std::string type;
    std::uint32_t offset = 0;

    friend bool operator==(const PushConstantField&, const PushConstantField&) = default;
};

struct PushConstants {
    std::uint32_t size = 0;
    std::vector<PushConstantField> fields;

    friend bool operator==(const PushConstants&, const PushConstants&) = default;
};

// Section 3 `tests`: a smoke/golden test reference. `golden` is empty when the entry has none.
struct ShaderTest {
    std::string name;
    std::string golden;

    friend bool operator==(const ShaderTest&, const ShaderTest&) = default;
};

struct ShaderEntry {
    std::string name;
    ShaderKind kind = ShaderKind::Element;
    std::vector<ShaderStage> stages; // declaration order, no duplicates, never Compute
    std::vector<StageEntry> entries; // exactly one per declared stage, in `stages` order
    std::vector<std::string> vulkan_sources;
    std::vector<std::string> webgpu_sources; // empty for a Vulkan-only element
    std::vector<VertexAttribute> vertex_layout;
    std::vector<ShaderBinding> bindings;
    std::optional<PushConstants> push_constants;
    std::vector<ShaderTest> tests;

    friend bool operator==(const ShaderEntry&, const ShaderEntry&) = default;
};

struct Manifest {
    std::uint32_t version = kManifestVersion;
    std::vector<ShaderEntry> shaders;

    friend bool operator==(const Manifest&, const Manifest&) = default;
};

// Stable validation failures (section 9). The path is the JSON field path; `detail` is a static
// description of the rule, never a formatted message, so error equality is deterministic.
enum class ValidationErrorCode : std::uint8_t {
    InvalidJson,
    JsonTooDeep,
    FileTooLarge,
    FileUnreadable,
    RootNotObject,
    MissingKey,
    UnknownKey,
    WrongType,
    OutOfRange,
    InvalidValue,
    UnsupportedVersion,
    UnknownStage,
    DuplicateStage,
    ComputeStageUnsupported,
    EntryStageNotDeclared,
    DuplicateShaderName,
    DuplicateVertexLocation,
    DuplicateVertexSemantic,
    VertexLayoutRequired,
    VertexLayoutNotAllowed,
    DuplicateBinding,
    InvalidPushConstants,
    DuplicateTestName,
    ViewTransitionInterface,
};

struct ValidationError {
    ValidationErrorCode code = ValidationErrorCode::InvalidJson;
    std::string path;        // "$", "$.manifestVersion", "$.shaders[0].stages[1]"
    std::string_view detail; // static literal; why the value was rejected
    std::size_t offset = 0;  // byte offset for InvalidJson/JsonTooDeep, otherwise 0

    friend bool operator==(const ValidationError&, const ValidationError&) = default;
};

// Parses and validates one manifest document. Text larger than kMaxManifestBytes is rejected
// before decoding. The returned value is fully validated: sections 1-4 rules, section 9 limits.
[[nodiscard]] std::expected<Manifest, ValidationError> parse_manifest(std::string_view json_text);

// Reads `path` (bounded by kMaxManifestBytes) and validates it. A missing/unreadable file is
// `FileUnreadable`; an oversized file is `FileTooLarge` without reading its contents.
[[nodiscard]] std::expected<Manifest, ValidationError> load_manifest_file(std::string_view path);

[[nodiscard]] std::string_view to_string(ValidationErrorCode code) noexcept;
[[nodiscard]] std::string_view to_string(ShaderKind kind) noexcept;
[[nodiscard]] std::string_view to_string(ShaderStage stage) noexcept;
[[nodiscard]] std::string_view to_string(ShaderBindingType type) noexcept;

} // namespace tessera::schema
