// SPDX-License-Identifier: MIT

#pragma once

#include "json.hpp"
#include "tessera/schema/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

// Private implementation contract for the shader manifest validator. The section 9 limits live
// here so the public header stays small; docs/specs/shader-manifest-v1.md section 9 is normative.
namespace tessera::schema::internal {

inline constexpr std::size_t kMaxShaderCount = 1024;
inline constexpr std::size_t kMaxStageCount = 3;
inline constexpr std::size_t kMaxSourceCount = 8;
inline constexpr std::size_t kMaxVertexAttributeCount = 32;
inline constexpr std::size_t kMaxBindingCount = 32;
inline constexpr std::size_t kMaxPushConstantFieldCount = 32;
inline constexpr std::size_t kMaxTestCount = 64;

inline constexpr std::size_t kMaxShaderNameLength = 128;
inline constexpr std::size_t kMaxEntryPointLength = 64;
inline constexpr std::size_t kMaxSourceNameLength = 128;
inline constexpr std::size_t kMaxFormatLength = 32;
inline constexpr std::size_t kMaxSemanticLength = 64;
inline constexpr std::size_t kMaxBindingNameLength = 64;
inline constexpr std::size_t kMaxTestNameLength = 64;
inline constexpr std::size_t kMaxGoldenPathLength = 256;
inline constexpr std::size_t kMaxTypeNameLength = 32;

inline constexpr std::uint32_t kMaxVertexLocation = 31;
inline constexpr std::uint32_t kMaxBindingGroup = 3;
inline constexpr std::uint32_t kMaxBindingIndex = 255;
inline constexpr std::uint32_t kMaxPushConstantSize = 128;

[[nodiscard]] std::unexpected<ValidationError> fail(ValidationErrorCode code, std::string path,
                                                    std::string_view detail = {});

[[nodiscard]] std::string member_path(std::string_view path, std::string_view key);
[[nodiscard]] std::string element_path(std::string_view path, std::size_t index);

[[nodiscard]] const JsonValue* find_member(const JsonValue& object, std::string_view key);

// Section 9: every object in the manifest rejects keys outside its closed set, so a typo or a
// field from a newer schema fails the build instead of being ignored.
[[nodiscard]] std::expected<void, ValidationError>
check_keys(const JsonValue& object, std::span<const std::string_view> allowed,
           std::string_view path);

// Shared field readers. They reject a wrong JSON type before the range checks.
[[nodiscard]] std::expected<const JsonValue*, ValidationError>
required_member(const JsonValue& object, std::string_view key, std::string_view object_path);

[[nodiscard]] std::expected<std::string, ValidationError>
string_value(const JsonValue& value, std::string_view path, std::size_t max_length);

[[nodiscard]] std::expected<std::string, ValidationError>
required_string(const JsonValue& object, std::string_view key, std::string_view object_path,
                std::size_t max_length);

[[nodiscard]] std::expected<std::uint32_t, ValidationError>
integer_value(const JsonValue& value, std::string_view path, std::uint32_t max_value);

[[nodiscard]] std::expected<std::uint32_t, ValidationError>
required_integer(const JsonValue& object, std::string_view key, std::string_view object_path,
                 std::uint32_t max_value);

// Section 3 fields with their own element shapes and bounds.
[[nodiscard]] std::expected<void, ValidationError>
parse_vertex_layout(const JsonValue& value, std::string_view path, ShaderEntry& entry);
[[nodiscard]] std::expected<void, ValidationError>
parse_bindings(const JsonValue& value, std::string_view path, ShaderEntry& entry);
[[nodiscard]] std::expected<void, ValidationError>
parse_push_constants(const JsonValue& value, std::string_view path, ShaderEntry& entry);
[[nodiscard]] std::expected<void, ValidationError>
parse_tests(const JsonValue& value, std::string_view path, ShaderEntry& entry);

// Byte size of a push-constant field type, or 0 when the token is not in the allowed set.
[[nodiscard]] std::uint32_t push_constant_type_size(std::string_view type) noexcept;

// One shader entry after shape validation. `has_vertex_layout` distinguishes an omitted key from
// an empty array, which section 3.1 treats differently.
struct ParsedShaderEntry {
    ShaderEntry entry;
    bool has_vertex_layout = false;
};

[[nodiscard]] std::expected<ParsedShaderEntry, ValidationError>
parse_shader_entry(const JsonValue& value, std::string_view path);

// Section 4 compatibility rules and the section 3.1 interface checks. Runs after the entry shape
// is parsed; `path` points at the shader object.
[[nodiscard]] std::expected<void, ValidationError> apply_shader_rules(ParsedShaderEntry& parsed,
                                                                      std::string_view path);

} // namespace tessera::schema::internal
