// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::gpu {

// The per-frame clip-record buffer (ADR-068 slice 3, issue #671).
//
// One `BufferUsage::Storage` buffer per frame in flight, rotated at frame start the same way the
// vertex arena rotates its slots: a slot is only reused once `frames_in_flight` frames have
// completed, so a frame never rewrites a buffer a submission still reads.
//
// Uploads are incremental. A frame interns sets as it pushes clips, and both the record area and
// the set-range area are append-only within a frame, so each upload writes only what was added
// since the last one. A frame that pushes no clips uploads nothing at all.
//
// The buffer is created lazily on the first upload, so a device without the capability (or a frame
// that never clips) allocates nothing.
class ClipBuffer {
public:
    ClipBuffer(rhi::Device& device, std::uint32_t frames_in_flight);
    ~ClipBuffer();

    ClipBuffer(const ClipBuffer&) = delete;
    ClipBuffer& operator=(const ClipBuffer&) = delete;

    // True when this device can bind a storage buffer, so the renderer may use the record path.
    // False means the caller keeps the push-constant path (ADR-068 section 5).
    [[nodiscard]] bool supported() const;

    // Rotates to the next slot and forgets what the previous frame uploaded. Called at frame start.
    void begin_frame();

    // Uploads `records` and `sets` for the current frame, writing only what changed since the last
    // call. Returns the buffer to bind, or nullptr when there is nothing to bind (no clips this
    // frame, or the device has no storage buffers).
    //
    // `records` is the frame's full record list, `sets` its full set list and `vertices` the
    // polygon vertex pool (ADR-068 slice 5); the class tracks how much of each it has already
    // written. The records and the vertices must be append-only within a frame, which is what
    // ClipStateTable guarantees.
    [[nodiscard]] rhi::Buffer* upload(const std::vector<clip::ClipRecord>& records,
                                      const std::vector<clip::ClipSetRange>& sets,
                                      const std::vector<clip::ClipVertex>& vertices);

    // The buffer the current frame last uploaded, or nullptr. The renderer binds this before a
    // draw.
    [[nodiscard]] rhi::Buffer* buffer() const {
        return current_ != nullptr ? current_->buffer.get() : nullptr;
    }

private:
    struct Slot {
        std::unique_ptr<rhi::Buffer> buffer;
        // How much of the frame's record and set lists this slot already holds.
        std::uint32_t uploaded_records = 0;
        std::uint32_t uploaded_sets = 0;
        std::uint32_t uploaded_vertices = 0;
    };

    void ensure_capacity(Slot& slot);

    rhi::Device* device_ = nullptr;
    std::vector<Slot> slots_;
    std::uint32_t slot_index_ = 0;
    Slot* current_ = nullptr;
};

} // namespace tessera::rendering::gpu
