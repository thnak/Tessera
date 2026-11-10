// SPDX-License-Identifier: MIT

// Stable names for manifest enums. They are diagnostic tokens: build logs and tests compare them,
// so existing spellings never change (docs/specs/shader-manifest-v1.md section 9).

#include "tessera/schema/manifest.hpp"

namespace tessera::schema {

std::string_view to_string(ValidationErrorCode code) noexcept {
    switch (code) {
    case ValidationErrorCode::InvalidJson:
        return "invalid_json";
    case ValidationErrorCode::JsonTooDeep:
        return "json_too_deep";
    case ValidationErrorCode::FileTooLarge:
        return "file_too_large";
    case ValidationErrorCode::FileUnreadable:
        return "file_unreadable";
    case ValidationErrorCode::RootNotObject:
        return "root_not_object";
    case ValidationErrorCode::MissingKey:
        return "missing_key";
    case ValidationErrorCode::UnknownKey:
        return "unknown_key";
    case ValidationErrorCode::WrongType:
        return "wrong_type";
    case ValidationErrorCode::OutOfRange:
        return "out_of_range";
    case ValidationErrorCode::InvalidValue:
        return "invalid_value";
    case ValidationErrorCode::UnsupportedVersion:
        return "unsupported_version";
    case ValidationErrorCode::UnknownStage:
        return "unknown_stage";
    case ValidationErrorCode::DuplicateStage:
        return "duplicate_stage";
    case ValidationErrorCode::ComputeStageUnsupported:
        return "compute_stage_unsupported";
    case ValidationErrorCode::EntryStageNotDeclared:
        return "entry_stage_not_declared";
    case ValidationErrorCode::DuplicateShaderName:
        return "duplicate_shader_name";
    case ValidationErrorCode::DuplicateVertexLocation:
        return "duplicate_vertex_location";
    case ValidationErrorCode::DuplicateVertexSemantic:
        return "duplicate_vertex_semantic";
    case ValidationErrorCode::VertexLayoutRequired:
        return "vertex_layout_required";
    case ValidationErrorCode::VertexLayoutNotAllowed:
        return "vertex_layout_not_allowed";
    case ValidationErrorCode::DuplicateBinding:
        return "duplicate_binding";
    case ValidationErrorCode::InvalidPushConstants:
        return "invalid_push_constants";
    case ValidationErrorCode::DuplicateTestName:
        return "duplicate_test_name";
    case ValidationErrorCode::ViewTransitionInterface:
        return "view_transition_interface";
    }
    return "invalid_json";
}

std::string_view to_string(ShaderKind kind) noexcept {
    switch (kind) {
    case ShaderKind::Element:
        return "element";
    case ShaderKind::ViewTransitionEffect:
        return "view-transition-effect";
    }
    return "element";
}

std::string_view to_string(ShaderStage stage) noexcept {
    switch (stage) {
    case ShaderStage::Vertex:
        return "vertex";
    case ShaderStage::Fragment:
        return "fragment";
    case ShaderStage::Compute:
        return "compute";
    }
    return "vertex";
}

std::string_view to_string(ShaderBindingType type) noexcept {
    switch (type) {
    case ShaderBindingType::UniformBuffer:
        return "uniform_buffer";
    case ShaderBindingType::StorageBuffer:
        return "storage_buffer";
    case ShaderBindingType::Sampler:
        return "sampler";
    case ShaderBindingType::SampledTexture:
        return "sampled_texture";
    case ShaderBindingType::StorageTexture:
        return "storage_texture";
    }
    return "uniform_buffer";
}

} // namespace tessera::schema
