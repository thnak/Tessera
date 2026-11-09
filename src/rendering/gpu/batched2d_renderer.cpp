// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace tessera::rendering::gpu {
namespace {

std::uint32_t pack_color(rhi::Color color) {
    const auto channel = [](float value) {
        const float clamped = std::clamp(value, 0.0f, 1.0f);
        return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
    };
    return channel(color.r) | (channel(color.g) << 8) | (channel(color.b) << 16) |
           (channel(color.a) << 24);
}

} // namespace

Batched2DRenderer::Batched2DRenderer(rhi::Device& device, rhi::Pipeline& pipeline,
                                     rhi::Texture& white_texture, Batched2DConfig config)
    : device_(&device),
      pipeline_(&pipeline),
      white_texture_(&white_texture),
      config_(config),
      current_texture_(&white_texture) {
    static_assert(sizeof(Vertex2D) == kVertex2DStride, "vertex stride must match the layout");

    const std::uint32_t capacity = std::max(config.initial_vertex_capacity, 64u);
    slots_.resize(std::max(config.frames_in_flight, 1u));
    for (FrameSlot& slot : slots_) {
        slot.capacity = capacity;
        slot.buffers.push_back(device_->create_buffer(
            {static_cast<std::size_t>(capacity) * kVertex2DStride, rhi::BufferUsage::Vertex}));
    }
    // The first frame advances to slot 0.
    slot_index_ = static_cast<std::uint32_t>(slots_.size()) - 1;
    vertices_.reserve(capacity);
    batches_.reserve(64);
    damage_regions_.reserve(8);
}

Batched2DRenderer::~Batched2DRenderer() = default;

void Batched2DRenderer::begin(rhi::CommandList& commands, const rhi::RenderTarget& target,
                              const rhi::RenderTargetDesc& desc) {
    begin_frame(commands, target, desc, nullptr);
}

void Batched2DRenderer::begin_damaged(rhi::CommandList& commands, const rhi::RenderTarget& target,
                                      const rhi::RenderTargetDesc& desc, const RedrawPlan& plan) {
    if (plan.full_redraw) {
        begin_frame(commands, target, desc, nullptr);
        return;
    }
    if (plan.scissors.empty()) {
        // A no-op frame reports no work rather than stale counters from a previous frame.
        commands_ = nullptr;
        in_frame_ = false;
        draw_calls_ = 0;
        emitted_vertices_ = 0;
        return;
    }
    begin_frame(commands, target, desc, &plan.scissors);
}

void Batched2DRenderer::begin_frame(rhi::CommandList& commands, const rhi::RenderTarget& target,
                                    const rhi::RenderTargetDesc& desc,
                                    const std::vector<rhi::Scissor>* damage_regions) {
    select_next_slot();
    commands_ = &commands;
    extent_ = desc.extent;
    base_scissor_ = rhi::Scissor{0, 0, static_cast<std::uint32_t>(desc.extent.width),
                                 static_cast<std::uint32_t>(desc.extent.height)};
    scissor_ = base_scissor_;
    scissor_stack_.clear();
    damage_regions_.clear();
    if (damage_regions != nullptr) {
        damage_regions_ = *damage_regions;
    }
    transform_ = rhi::Mat3::identity();
    transform_stack_.clear();
    vertices_.clear();
    batches_.clear();
    current_texture_ = white_texture_;
    current_batch_ = Batch{scissor_, current_texture_, 0, 0};
    draw_calls_ = 0;
    emitted_vertices_ = 0;
    in_frame_ = true;

    commands_->begin_render_pass(target, desc);
    commands_->bind_pipeline(*pipeline_);
    commands_->set_viewport(
        rhi::Viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height)});
    commands_->set_scissor(scissor_);
    update_push_constants();
}

void Batched2DRenderer::end() {
    if (!in_frame_) {
        return;
    }
    flush();
    commands_->end_render_pass();
    commands_ = nullptr;
    in_frame_ = false;
}

void Batched2DRenderer::set_viewport(rhi::Extent2D extent) {
    if (!in_frame_) {
        return;
    }
    flush();
    extent_ = extent;
    commands_->set_viewport(
        rhi::Viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height)});
    update_push_constants();
}

void Batched2DRenderer::set_scissor(const rhi::Scissor& scissor) {
    flush_batch();
    scissor_ = scissor;
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::push_scissor(const rhi::Scissor& scissor) {
    flush_batch();
    scissor_stack_.push_back(scissor_);
    scissor_ = rhi::intersect(scissor_, scissor);
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::pop_scissor() {
    flush_batch();
    if (!scissor_stack_.empty()) {
        scissor_ = scissor_stack_.back();
        scissor_stack_.pop_back();
    } else {
        scissor_ = base_scissor_;
    }
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::set_transform(const rhi::Mat3& transform) {
    transform_ = transform;
}

void Batched2DRenderer::push_transform(const rhi::Mat3& transform) {
    transform_stack_.push_back(transform_);
    transform_ = transform_ * transform;
}

void Batched2DRenderer::pop_transform() {
    if (!transform_stack_.empty()) {
        transform_ = transform_stack_.back();
        transform_stack_.pop_back();
    } else {
        transform_ = rhi::Mat3::identity();
    }
}

void Batched2DRenderer::push_rect(const rhi::Rect& rect, rhi::Color color) {
    emit_quad(rect, rhi::Rect{}, color, 0, 0, white_texture_);
}

void Batched2DRenderer::push_rounded_rect(const rhi::Rect& rect, rhi::Color color, float radius) {
    const float max_radius = std::min(rect.width, rect.height) * 0.5f;
    const float clamped_radius = std::clamp(radius, 0.0f, max_radius);
    emit_quad(rect, rhi::Rect{}, color, clamped_radius, 1, white_texture_);
}

void Batched2DRenderer::push_texture(const rhi::Rect& rect, rhi::Texture& texture,
                                     const rhi::Rect& uv, rhi::Color color) {
    emit_quad(rect, uv, color, 0, 2, &texture);
}

void Batched2DRenderer::flush() {
    if (!in_frame_ || vertices_.empty()) {
        return;
    }

    flush_batch();

    // Append after whatever earlier flushes of this frame wrote: their draws are recorded but
    // not executed yet, so overwriting offset 0 would change what they draw.
    const auto count = static_cast<std::uint32_t>(vertices_.size());
    ensure_buffer_space(count);
    rhi::Buffer& buffer = current_buffer();
    const std::uint32_t base = buffer_used_;
    buffer.write(vertices_.data(), static_cast<std::size_t>(count) * kVertex2DStride,
                 static_cast<std::size_t>(base) * kVertex2DStride);
    buffer_used_ += count;
    commands_->bind_vertex_buffer(buffer);

    std::uint32_t emitted_draws = 0;
    if (damage_regions_.empty()) {
        for (const Batch& batch : batches_) {
            commands_->set_scissor(batch.scissor);
            commands_->bind_texture(0, *batch.texture);
            commands_->draw(batch.vertex_count, base + batch.first_vertex);
            ++emitted_draws;
        }
    } else {
        // Replay each batch once per damage region. Regions are disjoint, so a pixel is
        // never blended twice even when several regions intersect the same batch.
        for (const Batch& batch : batches_) {
            bool texture_bound = false;
            for (const rhi::Scissor& region : damage_regions_) {
                const rhi::Scissor clipped = rhi::intersect(batch.scissor, region);
                if (clipped.width == 0 || clipped.height == 0) {
                    continue;
                }
                if (!texture_bound) {
                    commands_->bind_texture(0, *batch.texture);
                    texture_bound = true;
                }
                commands_->set_scissor(clipped);
                commands_->draw(batch.vertex_count, base + batch.first_vertex);
                ++emitted_draws;
            }
        }
    }

    draw_calls_ += emitted_draws;
    vertices_.clear();
    batches_.clear();
    current_batch_ = Batch{scissor_, current_texture_, 0, 0};
}

void Batched2DRenderer::select_next_slot() {
    slot_index_ = (slot_index_ + 1) % static_cast<std::uint32_t>(slots_.size());
    FrameSlot& slot = slots_[slot_index_];
    // Reusing a slot means the frame that last used it is complete (see frames_in_flight), so
    // buffers added for growth can go; the largest one is kept.
    if (slot.buffers.size() > 1) {
        slot.buffers.erase(slot.buffers.begin(), slot.buffers.end() - 1);
    }
    buffer_used_ = 0;
}

void Batched2DRenderer::ensure_buffer_space(std::size_t vertices) {
    FrameSlot& slot = slots_[slot_index_];
    if (buffer_used_ + vertices <= slot.capacity) {
        return;
    }

    // Draws already recorded this frame reference the current buffer, so it cannot be replaced
    // or destroyed; the rest of the frame continues in a larger buffer.
    std::uint32_t new_capacity = slot.capacity * 2;
    while (new_capacity < vertices) {
        new_capacity *= 2;
    }
    slot.buffers.push_back(device_->create_buffer(
        {static_cast<std::size_t>(new_capacity) * kVertex2DStride, rhi::BufferUsage::Vertex}));
    slot.capacity = new_capacity;
    buffer_used_ = 0;
}

rhi::Buffer& Batched2DRenderer::current_buffer() {
    return *slots_[slot_index_].buffers.back();
}

void Batched2DRenderer::flush_batch() {
    if (current_batch_.vertex_count == 0) {
        return;
    }

    batches_.push_back(current_batch_);
    current_batch_.first_vertex = static_cast<std::uint32_t>(vertices_.size());
    current_batch_.vertex_count = 0;
    current_batch_.scissor = scissor_;
    current_batch_.texture = current_texture_;
}

void Batched2DRenderer::emit_quad(const rhi::Rect& rect, const rhi::Rect& uv, rhi::Color color,
                                  float radius, float mode, rhi::Texture* texture) {
    if (!in_frame_) {
        return;
    }
    if (texture != current_texture_) {
        flush_batch();
        current_texture_ = texture;
        current_batch_.texture = texture;
    }

    const float x0 = rect.x;
    const float y0 = rect.y;
    const float x1 = rect.right();
    const float y1 = rect.bottom();
    const float half_width = rect.width * 0.5f;
    const float half_height = rect.height * 0.5f;

    const float u0 = uv.x;
    const float v0 = uv.y;
    const float u1 = uv.x + uv.width;
    const float v1 = uv.y + uv.height;

    const float positions[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    const float locals[4][2] = {{-half_width, -half_height},
                                {half_width, -half_height},
                                {half_width, half_height},
                                {-half_width, half_height}};
    const float texcoords[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
    const std::uint32_t packed_color = pack_color(color);
    constexpr std::uint32_t kIndices[6] = {0, 1, 2, 0, 2, 3};

    for (std::uint32_t index : kIndices) {
        float px = 0;
        float py = 0;
        transform_.transform_point(positions[index][0], positions[index][1], px, py);

        Vertex2D vertex;
        vertex.x = px;
        vertex.y = py;
        vertex.u = texcoords[index][0];
        vertex.v = texcoords[index][1];
        vertex.color = packed_color;
        vertex.local_x = locals[index][0];
        vertex.local_y = locals[index][1];
        vertex.half_width = half_width;
        vertex.half_height = half_height;
        vertex.radius = radius;
        vertex.mode = mode;
        vertices_.push_back(vertex);
    }

    current_batch_.vertex_count += 6;
    emitted_vertices_ += 6;
}

void Batched2DRenderer::update_push_constants() {
    Batched2DPushConstants push;
    push.viewport_width = static_cast<float>(extent_.width);
    push.viewport_height = static_cast<float>(extent_.height);
    push.aa_radius = (config_.enable_aa && !config_.golden_mode) ? 1.0f : 0.0f;
    commands_->push_constants(&push, sizeof(push));
}

} // namespace tessera::rendering::gpu
