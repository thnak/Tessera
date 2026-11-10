// SPDX-License-Identifier: MIT

// Section 4 compatibility rules for the shader manifest: stage coverage, duplicate declarations,
// vertex layouts, binding/layout expressibility, push-constant layout, test uniqueness, and the
// fixed interface of a view-transition effect (section 3.1). Compute stages and unknown stages are
// rejected earlier, when the stage token is read, so they carry their own typed errors.

#include "shader_manifest_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace tessera::schema::internal {
namespace {

bool has_stage(const ShaderEntry& entry, ShaderStage stage) {
    return std::ranges::find(entry.stages, stage) != entry.stages.end();
}

std::expected<void, ValidationError> check_duplicate_stages(const ShaderEntry& entry,
                                                            std::string_view path) {
    const std::string stages_path = member_path(path, "stages");
    for (std::size_t index = 0; index < entry.stages.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entry.stages[index] == entry.stages[previous]) {
                return fail(ValidationErrorCode::DuplicateStage, element_path(stages_path, index),
                            "stage is declared twice");
            }
        }
    }
    return {};
}

std::expected<void, ValidationError> check_duplicate_bindings(const ShaderEntry& entry,
                                                              std::string_view path) {
    const std::string bindings_path = member_path(path, "bindings");
    for (std::size_t index = 0; index < entry.bindings.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entry.bindings[index].group == entry.bindings[previous].group &&
                entry.bindings[index].binding == entry.bindings[previous].binding) {
                return fail(ValidationErrorCode::DuplicateBinding,
                            member_path(element_path(bindings_path, index), "binding"),
                            "group/binding pair is declared twice");
            }
        }
    }
    return {};
}

std::expected<void, ValidationError> check_duplicate_tests(const ShaderEntry& entry,
                                                           std::string_view path) {
    const std::string tests_path = member_path(path, "tests");
    for (std::size_t index = 0; index < entry.tests.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entry.tests[index].name == entry.tests[previous].name) {
                return fail(ValidationErrorCode::DuplicateTestName,
                            member_path(element_path(tests_path, index), "name"),
                            "test name is declared twice");
            }
        }
    }
    return {};
}

std::expected<void, ValidationError> check_vertex_layout(const ParsedShaderEntry& parsed,
                                                         std::string_view path) {
    const ShaderEntry& entry = parsed.entry;
    const std::string layout_path = member_path(path, "vertexLayout");
    if (entry.kind == ShaderKind::ViewTransitionEffect) {
        if (!entry.vertex_layout.empty()) {
            return fail(ValidationErrorCode::VertexLayoutNotAllowed, layout_path,
                        "a view-transition effect has no vertex buffers");
        }
        return {};
    }

    const bool vertex_stage = has_stage(entry, ShaderStage::Vertex);
    if (vertex_stage && !parsed.has_vertex_layout) {
        return fail(ValidationErrorCode::VertexLayoutRequired, layout_path,
                    "a vertex stage requires an explicit vertex layout");
    }
    if (!vertex_stage && parsed.has_vertex_layout && !entry.vertex_layout.empty()) {
        return fail(ValidationErrorCode::VertexLayoutNotAllowed, layout_path,
                    "only a vertex stage may declare a vertex layout");
    }
    for (std::size_t index = 0; index < entry.vertex_layout.size(); ++index) {
        const std::string attribute_path = element_path(layout_path, index);
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (entry.vertex_layout[index].location == entry.vertex_layout[previous].location) {
                return fail(ValidationErrorCode::DuplicateVertexLocation,
                            member_path(attribute_path, "location"),
                            "vertex location is declared twice");
            }
            if (entry.vertex_layout[index].semantic == entry.vertex_layout[previous].semantic) {
                return fail(ValidationErrorCode::DuplicateVertexSemantic,
                            member_path(attribute_path, "semantic"),
                            "vertex semantic is declared twice");
            }
        }
    }
    return {};
}

std::expected<void, ValidationError> check_push_constant_layout(const ShaderEntry& entry,
                                                                std::string_view path) {
    if (!entry.push_constants) {
        return {};
    }
    if (entry.kind == ShaderKind::ViewTransitionEffect) {
        return fail(ValidationErrorCode::ViewTransitionInterface,
                    member_path(path, "pushConstants"),
                    "a view-transition effect must not declare push constants");
    }
    const PushConstants& constants = *entry.push_constants;
    const std::string constants_path = member_path(path, "pushConstants");
    if (constants.size % 4 != 0) {
        return fail(ValidationErrorCode::InvalidPushConstants, member_path(constants_path, "size"),
                    "size must be a multiple of 4");
    }
    for (std::size_t index = 0; index < constants.fields.size(); ++index) {
        const PushConstantField& field = constants.fields[index];
        const std::string field_path = element_path(member_path(constants_path, "fields"), index);
        if (field.offset % 4 != 0) {
            return fail(ValidationErrorCode::InvalidPushConstants,
                        member_path(field_path, "offset"), "offset must be a multiple of 4");
        }
        const std::uint32_t field_size = push_constant_type_size(field.type);
        if (field.offset > constants.size || field_size > constants.size - field.offset) {
            return fail(ValidationErrorCode::InvalidPushConstants,
                        member_path(field_path, "offset"), "field exceeds the block size");
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const PushConstantField& other = constants.fields[previous];
            const std::uint32_t other_size = push_constant_type_size(other.type);
            const bool overlaps = field.offset < other.offset + other_size &&
                                  other.offset < field.offset + field_size;
            if (overlaps) {
                return fail(ValidationErrorCode::InvalidPushConstants,
                            member_path(field_path, "offset"), "push-constant fields overlap");
            }
            if (field.name == other.name) {
                return fail(ValidationErrorCode::InvalidPushConstants,
                            member_path(field_path, "name"),
                            "push-constant field name is declared twice");
            }
        }
    }
    return {};
}

struct ExpectedBinding {
    std::uint32_t group = 0;
    std::uint32_t binding = 0;
    ShaderBindingType type = ShaderBindingType::UniformBuffer;
    std::string_view name;
};

bool matches_transition_interface(const ShaderEntry& entry) {
    constexpr ExpectedBinding kExpected[] = {
        {0, 0, ShaderBindingType::UniformBuffer, "transition"},
        {0, 1, ShaderBindingType::Sampler, "samp"},
        {0, 2, ShaderBindingType::SampledTexture, "old"},
        {0, 3, ShaderBindingType::SampledTexture, "new"},
    };
    if (entry.bindings.size() != std::size(kExpected)) {
        return false;
    }
    for (const ExpectedBinding& expected : kExpected) {
        const auto found = std::ranges::find_if(entry.bindings, [&](const ShaderBinding& binding) {
            return binding.group == expected.group && binding.binding == expected.binding &&
                   binding.type == expected.type && binding.name == expected.name;
        });
        if (found == entry.bindings.end()) {
            return false;
        }
    }
    return true;
}

std::expected<void, ValidationError> check_view_transition_interface(const ShaderEntry& entry,
                                                                     std::string_view path) {
    const bool stages_match = entry.stages.size() == 2 && entry.stages[0] == ShaderStage::Vertex &&
                              entry.stages[1] == ShaderStage::Fragment;
    if (!stages_match) {
        return fail(ValidationErrorCode::ViewTransitionInterface, member_path(path, "stages"),
                    "stages must be [vertex, fragment]");
    }
    if (!matches_transition_interface(entry)) {
        return fail(ValidationErrorCode::ViewTransitionInterface, member_path(path, "bindings"),
                    "bindings must be the effect interface");
    }
    if (entry.webgpu_sources.empty()) {
        return fail(ValidationErrorCode::ViewTransitionInterface,
                    member_path(member_path(path, "sources"), "webgpu"),
                    "an effect requires WebGPU sources");
    }
    const bool has_golden = std::ranges::any_of(
        entry.tests, [](const ShaderTest& test) { return !test.golden.empty(); });
    if (!has_golden) {
        return fail(ValidationErrorCode::ViewTransitionInterface, member_path(path, "tests"),
                    "an effect requires at least one golden test");
    }
    return {};
}

} // namespace

std::expected<void, ValidationError> apply_shader_rules(ParsedShaderEntry& parsed,
                                                        std::string_view path) {
    ShaderEntry& entry = parsed.entry;
    if (auto result = check_duplicate_stages(entry, path); !result) {
        return result;
    }
    if (auto result = check_duplicate_bindings(entry, path); !result) {
        return result;
    }
    if (auto result = check_duplicate_tests(entry, path); !result) {
        return result;
    }
    if (auto result = check_vertex_layout(parsed, path); !result) {
        return result;
    }
    if (auto result = check_push_constant_layout(entry, path); !result) {
        return result;
    }
    if (entry.kind == ShaderKind::ViewTransitionEffect) {
        return check_view_transition_interface(entry, path);
    }
    return {};
}

} // namespace tessera::schema::internal
