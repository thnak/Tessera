// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/vertex_arena.hpp"

#include <algorithm>
#include <cstddef>

namespace tessera::rendering::gpu {
namespace {

// A tiny first buffer would make the first flush grow immediately, so keep a usable floor.
constexpr std::uint32_t kMinimumVertexCapacity = 64;

} // namespace

VertexArena::VertexArena(rhi::Device& device, std::uint32_t initial_capacity,
                         std::uint32_t frames_in_flight)
    : device_(&device) {
    const std::uint32_t capacity = std::max(initial_capacity, kMinimumVertexCapacity);
    slots_.resize(std::max(frames_in_flight, 1u));
    for (FrameSlot& slot : slots_) {
        slot.capacity = capacity;
        slot.buffers.push_back(device_->create_buffer(
            {static_cast<std::size_t>(capacity) * kVertex2DStride, rhi::BufferUsage::Vertex}));
    }
    // The first frame advances to slot 0.
    slot_index_ = static_cast<std::uint32_t>(slots_.size()) - 1;
    staged_.reserve(capacity);
}

VertexArena::~VertexArena() = default;

void VertexArena::begin_frame() {
    slot_index_ = (slot_index_ + 1) % static_cast<std::uint32_t>(slots_.size());
    FrameSlot& slot = slots_[slot_index_];
    // Reusing a slot means the frame that last used it is complete (see frames_in_flight), so
    // buffers added for growth can go; the largest one is kept.
    if (slot.buffers.size() > 1) {
        slot.buffers.erase(slot.buffers.begin(), slot.buffers.end() - 1);
    }
    buffer_used_ = 0;
    staged_.clear();
    submitted_vertices_ = 0;
}

void VertexArena::cancel_frame() {
    staged_.clear();
    submitted_vertices_ = 0;
}

void VertexArena::push_vertex(const Vertex2D& vertex) {
    staged_.push_back(vertex);
    ++submitted_vertices_;
}

void VertexArena::ensure_space(std::uint32_t vertices) {
    FrameSlot& slot = slots_[slot_index_];
    if (buffer_used_ + vertices <= slot.capacity) {
        return;
    }

    // Draws already recorded this frame reference the current buffer, so it cannot be replaced or
    // destroyed; the rest of the frame continues in a larger buffer.
    std::uint32_t new_capacity = slot.capacity * 2;
    while (new_capacity < vertices) {
        new_capacity *= 2;
    }
    slot.buffers.push_back(device_->create_buffer(
        {static_cast<std::size_t>(new_capacity) * kVertex2DStride, rhi::BufferUsage::Vertex}));
    slot.capacity = new_capacity;
    buffer_used_ = 0;
}

VertexArena::Span VertexArena::upload() {
    const auto count = static_cast<std::uint32_t>(staged_.size());
    if (count == 0) {
        return Span{};
    }
    ensure_space(count);
    rhi::Buffer& buffer = *slots_[slot_index_].buffers.back();
    const std::uint32_t base = buffer_used_;
    buffer.write(staged_.data(), static_cast<std::size_t>(count) * kVertex2DStride,
                 static_cast<std::size_t>(base) * kVertex2DStride);
    buffer_used_ += count;
    return Span{&buffer, base, count};
}

void VertexArena::clear() {
    staged_.clear();
}

} // namespace tessera::rendering::gpu
