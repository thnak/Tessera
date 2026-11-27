// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/clip_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::rendering::gpu {
namespace {

constexpr std::size_t kBufferBytes = gradient::total_buffer_bytes();

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
    current_->uploaded_vertices = 0;
    current_->uploaded_gradients = 0;
    current_->uploaded_stops = 0;
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
                                const std::vector<clip::ClipSetRange>& sets,
                                const std::vector<clip::ClipVertex>& vertices,
                                const GradientTable* gradients) {
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

    // The vertex pool is append-only within a frame, like the records, so only the vertices added
    // since the last upload are written. A polygon record's range points into it, which is why the
    // vertices go in before any draw that reads the set.
    const std::uint32_t total_vertices = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(vertices.size()), clip::kMaxClipVertices);
    if (total_vertices > current_->uploaded_vertices) {
        const std::size_t offset =
            clip::vertices_offset(clip::kMaxClipRecords, clip::kMaxClipSets) +
            static_cast<std::size_t>(current_->uploaded_vertices) * sizeof(clip::ClipVertex);
        const std::size_t count =
            (total_vertices - current_->uploaded_vertices) * sizeof(clip::ClipVertex);
        current_->buffer->write(vertices.data() + current_->uploaded_vertices, count, offset);
        current_->uploaded_vertices = total_vertices;
    }

    // The gradient area is append-only like the records: a record's `first_stop` points at stops
    // already written, so the stops go in first. Only what was added since the last upload is
    // written, and a frame with no gradients writes nothing here.
    if (gradients != nullptr) {
        const std::uint32_t total_stops =
            std::min<std::uint32_t>(gradients->stop_count(), gradient::kMaxGradientStops);
        if (total_stops > current_->uploaded_stops) {
            const std::uint32_t first = current_->uploaded_stops;
            const std::uint32_t added = total_stops - first;
            current_->buffer->write(
                gradients->stop_colors().data() + static_cast<std::size_t>(first) * 4,
                added * gradient::kStopColorBytes,
                gradient::stop_colors_offset() + first * gradient::kStopColorBytes);
            current_->buffer->write(
                gradients->stop_offsets().data() + first, added * gradient::kStopOffsetBytes,
                gradient::stop_offsets_offset() + first * gradient::kStopOffsetBytes);
            current_->uploaded_stops = total_stops;
        }
        const std::uint32_t total_gradients = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(gradients->records().size()), gradient::kMaxGradientRecords);
        if (total_gradients > current_->uploaded_gradients) {
            const std::uint32_t first = current_->uploaded_gradients;
            current_->buffer->write(gradients->records().data() + first,
                                    (total_gradients - first) * sizeof(gradient::GradientRecord),
                                    gradient::records_offset() +
                                        first * sizeof(gradient::GradientRecord));
            current_->uploaded_gradients = total_gradients;
        }
    }

    // The header carries the record and vertex counts, which the shader does not need (it reads a
    // set's range) but a debug readback does.
    clip::ClipBufferHeader header;
    header.record_count = current_->uploaded_records;
    header.vertex_count = current_->uploaded_vertices;
    header.gradient_count = current_->uploaded_gradients;
    header.gradient_stop_count = current_->uploaded_stops;
    current_->buffer->write(&header, sizeof(header), 0);

    return current_->buffer.get();
}

} // namespace tessera::rendering::gpu
