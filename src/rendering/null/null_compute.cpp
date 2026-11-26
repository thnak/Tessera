// SPDX-License-Identifier: MIT
//
// The null backend's compute commands (RHI 1.16/1.18, issue #684). The null backend executes no
// shader, so every call is validated the way the Vulkan backend validates it and then recorded into
// `NullCommandStats`; a structural test asserts the record. A list from a device without compute
// (`create_null_device`) throws the same error a backend without compute does.

#include "null_resources.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace tessera::rendering::null {
namespace {

// Matches the Vulkan backend's per-dimension dispatch limit.
constexpr std::uint32_t kMaxDispatchCount = 65535;

} // namespace

void NullCommandList::bind_compute_pipeline(const rhi::ComputePipeline& pipeline) {
    if (!compute_enabled_) {
        throw std::runtime_error("bind_compute_pipeline: this backend has no compute");
    }
    if (in_pass_) {
        throw std::logic_error(
            "null bind_compute_pipeline: compute is recorded outside a render pass");
    }
    const auto* null_pipeline = dynamic_cast<const NullComputePipeline*>(&pipeline);
    if (null_pipeline == nullptr) {
        throw std::invalid_argument(
            "null bind_compute_pipeline: the pipeline belongs to another backend");
    }
    compute_pipeline_ = null_pipeline;
    ++stats_.compute_pipeline_binds;
    stats_.command_order.push_back(NullCommandKind::BindComputePipeline);
}

void NullCommandList::bind_compute_buffer(std::uint32_t slot, const rhi::Buffer& buffer) {
    if (!compute_enabled_) {
        throw std::runtime_error("bind_compute_buffer: this backend has no compute");
    }
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("null bind_compute_buffer: missing compute pipeline");
    }
    if (slot >= compute_pipeline_->storage_buffer_count()) {
        throw std::invalid_argument("null bind_compute_buffer: slot " + std::to_string(slot) +
                                    " is outside the pipeline's storage buffers");
    }
    if (buffer.size() == 0) {
        throw std::invalid_argument("null bind_compute_buffer: an empty buffer cannot be bound");
    }
    const auto* null_buffer = dynamic_cast<const NullBuffer*>(&buffer);
    if (null_buffer == nullptr || null_buffer->usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "null bind_compute_buffer: the buffer was not created with BufferUsage::Storage");
    }
    ++stats_.compute_buffer_binds;
    stats_.command_order.push_back(NullCommandKind::BindComputeBuffer);
}

void NullCommandList::compute_push_constants(const void*, std::size_t size) {
    if (!compute_enabled_) {
        throw std::runtime_error("compute_push_constants: this backend has no compute");
    }
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("null compute_push_constants: missing compute pipeline");
    }
    if (size == 0 || size > compute_pipeline_->push_constant_size() || size % 4 != 0) {
        throw std::invalid_argument("null compute_push_constants: size " + std::to_string(size) +
                                    " does not fit the pipeline's push-constant block");
    }
    stats_.command_order.push_back(NullCommandKind::ComputePushConstants);
}

void NullCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    if (!compute_enabled_) {
        throw std::runtime_error("dispatch: this backend has no compute");
    }
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("null dispatch: missing compute pipeline");
    }
    if (x == 0 || y == 0 || z == 0 || x > kMaxDispatchCount || y > kMaxDispatchCount ||
        z > kMaxDispatchCount) {
        throw std::invalid_argument("null dispatch: workgroup counts must be 1.." +
                                    std::to_string(kMaxDispatchCount));
    }
    stats_.dispatches.push_back(NullDispatchRecord{x, y, z, false, 0});
    stats_.command_order.push_back(NullCommandKind::Dispatch);
}

void NullCommandList::dispatch_indirect(const rhi::Buffer& buffer, std::size_t offset) {
    if (!compute_enabled_) {
        throw std::runtime_error("dispatch_indirect: this backend has no compute");
    }
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("null dispatch_indirect: missing compute pipeline");
    }
    const auto* null_buffer = dynamic_cast<const NullBuffer*>(&buffer);
    if (null_buffer == nullptr || null_buffer->usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "null dispatch_indirect: the buffer was not created with BufferUsage::Storage");
    }
    constexpr std::size_t kArgumentBytes = 3 * sizeof(std::uint32_t);
    if (offset % 4 != 0 || offset > buffer.size() || buffer.size() - offset < kArgumentBytes) {
        throw std::invalid_argument("null dispatch_indirect: offset " + std::to_string(offset) +
                                    " is not 4-byte aligned with 12 bytes inside the buffer");
    }
    std::uint32_t counts[3] = {};
    buffer.read(counts, kArgumentBytes, offset);
    stats_.dispatches.push_back(NullDispatchRecord{counts[0], counts[1], counts[2], true, offset});
    stats_.command_order.push_back(NullCommandKind::DispatchIndirect);
}

void NullCommandList::compute_barrier() {
    if (!compute_enabled_) {
        throw std::runtime_error("compute_barrier: this backend has no compute");
    }
    ++stats_.compute_barriers;
    stats_.command_order.push_back(NullCommandKind::ComputeBarrier);
}

void NullCommandList::copy_buffer(const rhi::Buffer& source, rhi::Buffer& destination,
                                  std::size_t size) {
    if (!compute_enabled_) {
        throw std::runtime_error("copy_buffer: this backend has no compute");
    }
    if (size == 0 || size > source.size() || size > destination.size()) {
        throw std::invalid_argument("null copy_buffer: size " + std::to_string(size) +
                                    " does not fit both buffers");
    }
    // The null buffers keep real bytes, so the copy moves them: a test can stage data in a Storage
    // buffer and read it back from a Readback buffer without a GPU.
    std::vector<std::uint8_t> bytes(size);
    source.read(bytes.data(), size);
    destination.write(bytes.data(), size);
    ++stats_.buffer_copies;
    stats_.command_order.push_back(NullCommandKind::CopyBuffer);
}

} // namespace tessera::rendering::null
