// SPDX-License-Identifier: MIT
//
// The null backend's storage-buffer bind (RHI 1.12, ADR-068). Split out of null_device.cpp, which
// is at the 500-line hard limit: the bind is one responsibility (validate the request the way a
// real backend does, then record it) and the rest of that file is resource creation and the other
// commands.

#include "tessera/rendering/null/null_device.hpp"

#include <stdexcept>
#include <string>

namespace tessera::rendering::null {

void NullCommandList::bind_storage_buffer(std::uint32_t slot, const rhi::Buffer& buffer) {
    // Validating-backend parity: the null backend rejects the same misuse the Vulkan backend does,
    // so a structural test on it stays meaningful.
    if (current_pipeline_ == nullptr) {
        throw std::logic_error("null bind_storage_buffer: missing pipeline");
    }
    if (!current_pipeline_->declares_storage_buffer()) {
        throw std::logic_error(
            "null bind_storage_buffer: the bound pipeline declares no storage buffer");
    }
    // The storage buffer is set 1, so the only legal slot is 1 (ADR-068: a separate set from the
    // sampled image, so a shader can read both).
    if (slot != 1) {
        throw std::invalid_argument("null bind_storage_buffer: the storage buffer is set 1, not "
                                    "slot " +
                                    std::to_string(slot));
    }
    if (buffer.size() == 0) {
        throw std::invalid_argument("null bind_storage_buffer: an empty buffer cannot be read");
    }
    ++stats_.storage_buffer_binds;
    stats_.command_order.push_back(NullCommandKind::BindStorageBuffer);
    stats_.storage_buffer_records.push_back(NullStorageBufferBind{slot, buffer.size()});
}

} // namespace tessera::rendering::null
