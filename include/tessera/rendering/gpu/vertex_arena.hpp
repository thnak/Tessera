// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::gpu {

// Per-frame vertex memory for the batched 2D renderer (god-type audit slice 8).
//
// A frame stages its vertices on the CPU and every flush hands the staged range to upload(), which
// writes it after whatever an earlier flush of the same frame wrote and returns the base vertex
// index. Both halves matter: draws already recorded this frame reference the buffers written so
// far, so a frame that outgrows its buffer continues in a larger one rather than replacing or
// destroying it. The extra buffers are released when the slot rotates, which is the renderer's
// documented "frame F is complete" point (Batched2DConfig::frames_in_flight).
//
// The arena records no commands and knows no batch: it answers "where do these vertices live", and
// the renderer keeps the bind and the draw calls.
class VertexArena {
public:
    // `initial_capacity` is a vertex count (clamped to a small floor), `frames_in_flight` the
    // number of buffer slots to rotate through.
    VertexArena(rhi::Device& device, std::uint32_t initial_capacity,
                std::uint32_t frames_in_flight);
    ~VertexArena();

    VertexArena(const VertexArena&) = delete;
    VertexArena& operator=(const VertexArena&) = delete;

    // Where one uploaded range lives: the buffer to bind and the base vertex index to draw from.
    struct Span {
        rhi::Buffer* buffer = nullptr;
        std::uint32_t base_vertex = 0;
        std::uint32_t vertex_count = 0;
    };

    // Rotates to the next buffer slot, releasing the previous occupant's growth buffers. Doing this
    // at frame start (not frame end) is what lets a slot's buffers be freed safely: the slot is
    // only reused once `frames_in_flight` frames have completed.
    void begin_frame();

    // Drops the staged vertices and the counters without rotating the slot. For the "no work this
    // frame" path: the frame never writes, so it must not consume a slot (that would rotate the
    // in-flight window for a frame the GPU never saw).
    void cancel_frame();

    // Stages one vertex for the current frame.
    void push_vertex(const Vertex2D& vertex);

    // Writes every staged vertex to the current slot's buffer, after any earlier flush of this
    // frame, and returns the range. Does not bind or draw; the caller records that. The staged
    // vertices stay available until clear(), because batch offsets index into them.
    [[nodiscard]] Span upload();

    // Drops the staged vertices; call once the frame's batches have been drawn.
    void clear();

    [[nodiscard]] std::uint32_t staged_vertices() const {
        return static_cast<std::uint32_t>(staged_.size());
    }
    // Vertices submitted this frame, including those already uploaded (the renderer's
    // vertex_count() diagnostic).
    [[nodiscard]] std::uint32_t vertex_count() const {
        return submitted_vertices_;
    }

private:
    // One in-flight frame's vertex memory. A frame normally uses a single buffer; growth appends.
    struct FrameSlot {
        std::vector<std::unique_ptr<rhi::Buffer>> buffers;
        std::uint32_t capacity = 0; // vertices held by buffers.back()
    };

    void ensure_space(std::uint32_t vertices);

    rhi::Device* device_ = nullptr;
    std::vector<FrameSlot> slots_;
    std::uint32_t slot_index_ = 0;
    std::uint32_t buffer_used_ = 0; // vertices already written to the current buffer this frame
    std::vector<Vertex2D> staged_;
    std::uint32_t submitted_vertices_ = 0;
};

} // namespace tessera::rendering::gpu
