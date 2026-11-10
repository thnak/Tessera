// SPDX-License-Identifier: MIT

// Section 3 array fields of the shader manifest: vertex layouts, bindings, push constants and
// tests. Each parser owns its closed key set, required keys, value types and scalar bounds;
// cross-entry and compatibility rules run in shader_manifest_rules.cpp.

#include "shader_manifest_internal.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::schema::internal {
namespace {

constexpr std::string_view kVertexFormats[] = {"float", "float2", "float3", "float4", "ubyte4norm"};

bool is_vertex_format(std::string_view token) {
    for (std::string_view format : kVertexFormats) {
        if (format == token) {
            return true;
        }
    }
    return false;
}

std::optional<ShaderBindingType> binding_from_token(std::string_view token) {
    if (token == "uniform_buffer") {
        return ShaderBindingType::UniformBuffer;
    }
    if (token == "storage_buffer") {
        return ShaderBindingType::StorageBuffer;
    }
    if (token == "sampler") {
        return ShaderBindingType::Sampler;
    }
    if (token == "sampled_texture") {
        return ShaderBindingType::SampledTexture;
    }
    if (token == "storage_texture") {
        return ShaderBindingType::StorageTexture;
    }
    return std::nullopt;
}

std::expected<void, ValidationError>
parse_vertex_layout_impl(const JsonValue& value, std::string_view path, ShaderEntry& entry) {
    constexpr std::string_view kAttributeKeys[] = {"location", "format", "semantic"};
    const auto* array = value.as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an array");
    }
    if (array->size() > kMaxVertexAttributeCount) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path),
                    "too many vertex attributes");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(path, index);
        const JsonValue& item = (*array)[index];
        if (!item.is_object()) {
            return fail(ValidationErrorCode::WrongType, item_path, "expected an object");
        }
        if (auto keys = check_keys(item, kAttributeKeys, item_path); !keys) {
            return keys;
        }
        VertexAttribute attribute;
        auto location = required_integer(item, "location", item_path, kMaxVertexLocation);
        if (!location) {
            return std::unexpected(location.error());
        }
        attribute.location = *location;
        auto format = required_string(item, "format", item_path, kMaxFormatLength);
        if (!format) {
            return std::unexpected(format.error());
        }
        if (!is_vertex_format(*format)) {
            return fail(ValidationErrorCode::InvalidValue, member_path(item_path, "format"),
                        "unknown vertex format");
        }
        attribute.format = std::move(*format);
        auto semantic = required_string(item, "semantic", item_path, kMaxSemanticLength);
        if (!semantic) {
            return std::unexpected(semantic.error());
        }
        attribute.semantic = std::move(*semantic);
        entry.vertex_layout.push_back(std::move(attribute));
    }
    return {};
}

std::expected<void, ValidationError>
parse_bindings_impl(const JsonValue& value, std::string_view path, ShaderEntry& entry) {
    constexpr std::string_view kBindingKeys[] = {"group", "binding", "type", "name"};
    const auto* array = value.as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an array");
    }
    if (array->size() > kMaxBindingCount) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "too many bindings");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(path, index);
        const JsonValue& item = (*array)[index];
        if (!item.is_object()) {
            return fail(ValidationErrorCode::WrongType, item_path, "expected an object");
        }
        if (auto keys = check_keys(item, kBindingKeys, item_path); !keys) {
            return keys;
        }
        ShaderBinding binding;
        auto group = required_integer(item, "group", item_path, kMaxBindingGroup);
        if (!group) {
            return std::unexpected(group.error());
        }
        binding.group = *group;
        auto index_value = required_integer(item, "binding", item_path, kMaxBindingIndex);
        if (!index_value) {
            return std::unexpected(index_value.error());
        }
        binding.binding = *index_value;
        auto type = required_string(item, "type", item_path, kMaxTypeNameLength);
        if (!type) {
            return std::unexpected(type.error());
        }
        const auto parsed_type = binding_from_token(*type);
        if (!parsed_type) {
            return fail(ValidationErrorCode::InvalidValue, member_path(item_path, "type"),
                        "unknown binding type");
        }
        binding.type = *parsed_type;
        auto name = required_string(item, "name", item_path, kMaxBindingNameLength);
        if (!name) {
            return std::unexpected(name.error());
        }
        binding.name = std::move(*name);
        entry.bindings.push_back(std::move(binding));
    }
    return {};
}

std::expected<void, ValidationError>
parse_push_constants_impl(const JsonValue& value, std::string_view path, ShaderEntry& entry) {
    constexpr std::string_view kPushKeys[] = {"size", "fields"};
    constexpr std::string_view kFieldKeys[] = {"name", "type", "offset"};
    if (!value.is_object()) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an object");
    }
    if (auto keys = check_keys(value, kPushKeys, path); !keys) {
        return keys;
    }
    PushConstants constants;
    auto size = required_integer(value, "size", path, kMaxPushConstantSize);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size == 0) {
        return fail(ValidationErrorCode::InvalidValue, member_path(path, "size"),
                    "push constants need a positive size");
    }
    constants.size = *size;

    auto fields = required_member(value, "fields", path);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    const auto* array = (*fields)->as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, member_path(path, "fields"),
                    "expected an array");
    }
    if (array->size() > kMaxPushConstantFieldCount) {
        return fail(ValidationErrorCode::OutOfRange, member_path(path, "fields"),
                    "too many push-constant fields");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(member_path(path, "fields"), index);
        const JsonValue& item = (*array)[index];
        if (!item.is_object()) {
            return fail(ValidationErrorCode::WrongType, item_path, "expected an object");
        }
        if (auto keys = check_keys(item, kFieldKeys, item_path); !keys) {
            return keys;
        }
        PushConstantField field;
        auto name = required_string(item, "name", item_path, kMaxBindingNameLength);
        if (!name) {
            return std::unexpected(name.error());
        }
        field.name = std::move(*name);
        auto type = required_string(item, "type", item_path, kMaxTypeNameLength);
        if (!type) {
            return std::unexpected(type.error());
        }
        if (push_constant_type_size(*type) == 0) {
            return fail(ValidationErrorCode::InvalidValue, member_path(item_path, "type"),
                        "unknown push-constant type");
        }
        field.type = std::move(*type);
        auto offset = required_integer(item, "offset", item_path, kMaxPushConstantSize);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        field.offset = *offset;
        constants.fields.push_back(std::move(field));
    }
    entry.push_constants = std::move(constants);
    return {};
}

bool is_project_relative_path(std::string_view text) {
    if (text.empty() || text.front() == '/' || text.front() == '\\') {
        return false;
    }
    return text.find(':') == std::string_view::npos && text.find("..") == std::string_view::npos;
}

std::expected<void, ValidationError> parse_tests_impl(const JsonValue& value, std::string_view path,
                                                      ShaderEntry& entry) {
    constexpr std::string_view kTestKeys[] = {"name", "golden"};
    const auto* array = value.as_array();
    if (array == nullptr) {
        return fail(ValidationErrorCode::WrongType, std::string(path), "expected an array");
    }
    if (array->empty()) {
        return fail(ValidationErrorCode::InvalidValue, std::string(path),
                    "at least one test is required");
    }
    if (array->size() > kMaxTestCount) {
        return fail(ValidationErrorCode::OutOfRange, std::string(path), "too many tests");
    }
    for (std::size_t index = 0; index < array->size(); ++index) {
        const std::string item_path = element_path(path, index);
        const JsonValue& item = (*array)[index];
        if (!item.is_object()) {
            return fail(ValidationErrorCode::WrongType, item_path, "expected an object");
        }
        if (auto keys = check_keys(item, kTestKeys, item_path); !keys) {
            return keys;
        }
        ShaderTest test;
        auto name = required_string(item, "name", item_path, kMaxTestNameLength);
        if (!name) {
            return std::unexpected(name.error());
        }
        test.name = std::move(*name);
        if (const JsonValue* golden = find_member(item, "golden")) {
            auto golden_path =
                string_value(*golden, member_path(item_path, "golden"), kMaxGoldenPathLength);
            if (!golden_path) {
                return std::unexpected(golden_path.error());
            }
            if (!is_project_relative_path(*golden_path)) {
                return fail(ValidationErrorCode::InvalidValue, member_path(item_path, "golden"),
                            "golden path must be project-relative");
            }
            test.golden = std::move(*golden_path);
        }
        entry.tests.push_back(std::move(test));
    }
    return {};
}

} // namespace

std::expected<void, ValidationError>
parse_vertex_layout(const JsonValue& value, std::string_view path, ShaderEntry& entry) {
    return parse_vertex_layout_impl(value, path, entry);
}

std::expected<void, ValidationError> parse_bindings(const JsonValue& value, std::string_view path,
                                                    ShaderEntry& entry) {
    return parse_bindings_impl(value, path, entry);
}

std::expected<void, ValidationError>
parse_push_constants(const JsonValue& value, std::string_view path, ShaderEntry& entry) {
    return parse_push_constants_impl(value, path, entry);
}

std::expected<void, ValidationError> parse_tests(const JsonValue& value, std::string_view path,
                                                 ShaderEntry& entry) {
    return parse_tests_impl(value, path, entry);
}

} // namespace tessera::schema::internal
