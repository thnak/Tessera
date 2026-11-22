// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/clip_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::rendering::gpu {
namespace {

constexpr std::size_t kBufferBytes = clip::buffer_bytes(clip::kMaxClipRecords, clip::kMaxClipSets);

} // namespace

ClipBuffer::ClipBuffer(rhi::Device& device, std::uint32_t frames_in_flight)
    : device_(&device) {
    // At least one slot: a caller that asks for zero still needs somewhere to write this frame.
    slots_.resize(std::max<std::uint32_t>(frames_in_flight, 1U));
}

ClipBuffer::~ClipBuffer() = default;

bool ClipBuffer::supported() const {
    return device_->supports_storage_buffers();
}

void ClipBuffer::begin_frame() {
    slot_index_ = (slot_index_ + 1) % slots_.size();
    current_ = &slots_[slot_index_];
    // The slot's buffer is reused, but its contents are this frame's: the counters restart so the
    // first upload of the frame writes from offset zero.
    current_->uploaded_records = 0;
    current_->uploaded_sets = 0;
}

void ClipBuffer::ensure_capacity(Slot& slot) {
    if (slot.buffer != nullptr) {
        return;
    }
    // Created on first use, so a frame that never clips allocates nothing and a device without the
    // capability never reaches here (supported() gates the caller).
    slot.buffer = device_->create_buffer({kBufferBytes, rhi::BufferUsage::Storage});
}

rhi::Buffer* ClipBuffer::upload(const std::vector<clip::ClipRecord>& records,
                                const std::vector<clip::ClipSetRange>& sets) {
    if (current_ == nullptr || !supported()) {
        return nullptr;
    }
    // The pipeline declares set 1 statically, so a draw with nothing bound to it is a validation
    // error even when the shader's clip loop would not run. The buffer is therefore always created
    // and always bound; a frame with no clips writes only the empty set 0 and the header.
    ensure_capacity(*current_);
    if (current_->buffer == nullptr) {
        return nullptr;
    }

    // The record area is append-only within a frame, so only the records added since the last
    // upload are written. A set's range points at records that are already in the buffer, which is
    // why the records must be written before the sets that reference them.
    const std::uint32_t total_records =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(records.size()), clip::kMaxClipRecords);
    if (total_records > current_->uploaded_records) {
        const std::size_t offset =
            clip::records_offset() +
            static_cast<std::size_t>(current_->uploaded_records) * sizeof(clip::ClipRecord);
        const std::size_t count =
            (total_records - current_->uploaded_records) * sizeof(clip::ClipRecord);
        current_->buffer->write(records.data() + current_->uploaded_records, count, offset);
        current_->uploaded_records = total_records;
    }

    // The set ranges are rewritten wholesale: a set interned earlier in the frame can gain records
    // when a deeper clip folds into it, so its range is not append-only. Set 0 (the empty set) is
    // always written, so an unclipped draw reads a zero count rather than uninitialised bytes.
    const std::uint32_t total_sets = std::max<std::uint32_t>(
        1U, std::min<std::uint32_t>(static_cast<std::uint32_t>(sets.size()), clip::kMaxClipSets));
    if (total_sets > 0) {
        static const clip::ClipSetRange kEmpty{};
        const clip::ClipSetRange* source = sets.empty() ? &kEmpty : sets.data();
        current_->buffer->write(source, total_sets * sizeof(clip::ClipSetRange),
                                clip::sets_offset(clip::kMaxClipRecords));
        current_->uploaded_sets = total_sets;
    }

    // The header carries the record count, which the shader does not need (it reads a set's range)
    // but a debug readback and a future polygon slice do.
    clip::ClipBufferHeader header;
    header.record_count = current_->uploaded_records;
    current_->buffer->write(&header, sizeof(header), 0);

    return current_->buffer.get();
}

} // namespace tessera::rendering::gpu
