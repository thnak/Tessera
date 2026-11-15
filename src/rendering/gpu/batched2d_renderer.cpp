// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

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

bool same_scissor(const rhi::Scissor& a, const rhi::Scissor& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

} // namespace

Batched2DRenderer::Batched2DRenderer(rhi::Device& device, rhi::Pipeline& pipeline,
                                     rhi::Texture& white_texture, Batched2DConfig config,
                                     GlassPipelines glass)
    : device_(&device),
      pipeline_(&pipeline),
      white_texture_(&white_texture),
      // Declaration order, not readability order: the collaborators are declared above the frame
      // state, and -Wreorder is an error on the Linux lane.
      glass_(device, glass, config.max_offscreen_targets, config.glass),
      clips_(),
      vertices_(device, config.initial_vertex_capacity, config.frames_in_flight),
      config_(config),
      current_texture_(&white_texture) {
    static_assert(sizeof(Vertex2D) == kVertex2DStride, "vertex stride must match the layout");
    batches_.reserve(64);
    damage_regions_.reserve(8);
    transform_stack_.reserve(16);
    scissor_stack_.reserve(16);
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
        // A no-op frame reports no work rather than stale counters from a previous frame, and does
        // not rotate the vertex slot (no GPU work references it).
        commands_ = nullptr;
        in_frame_ = false;
        draw_calls_ = 0;
        vertices_.cancel_frame();
        return;
    }
    begin_frame(commands, target, desc, &plan.scissors);
}

void Batched2DRenderer::begin_frame(rhi::CommandList& commands, const rhi::RenderTarget& target,
                                    const rhi::RenderTargetDesc& desc,
                                    const std::vector<rhi::Scissor>* damage_regions) {
    // MSAA opt-in: the renderer never silently renders with a mismatched pipeline/target.
    const std::uint32_t target_samples = target.sample_count();
    if (desc.sample_count != target_samples || config_.sample_count != target_samples ||
        (pipeline_ != nullptr && pipeline_->sample_count() != target_samples)) {
        throw std::runtime_error(
            "batched2d: sample count mismatch (config " + std::to_string(config_.sample_count) +
            ", desc " + std::to_string(desc.sample_count) + ", target " +
            std::to_string(target_samples) + ", pipeline " +
            std::to_string(pipeline_ != nullptr ? pipeline_->sample_count() : 0) + ")");
    }
    vertices_.begin_frame();
    commands_ = &commands;
    frame_target_ = &target;
    frame_desc_ = desc;
    pass_broken_ = false;
    // The offscreen pool advances with the frame slots, so a target acquired in frame k is only
    // handed out again once the slot rotation proves frame k complete.
    ++frame_index_;
    glass_.begin_frame(frame_index_, config_.frames_in_flight, desc.format);
    extent_ = desc.extent;
    base_scissor_ = rhi::Scissor{0, 0, static_cast<std::uint32_t>(desc.extent.width),
                                 static_cast<std::uint32_t>(desc.extent.height)};
    scissor_ = base_scissor_;
    scissor_stack_.clear();
    damage_regions_.clear();
    if (damage_regions != nullptr) {
        damage_regions_ = *damage_regions;
    }
    clips_.reset();
    transform_ = rhi::Mat3::identity();
    transform_stack_.clear();
    content_scale_x_ = 1.0f;
    content_scale_y_ = 1.0f;
    batches_.clear();
    current_texture_ = white_texture_;
    current_batch_ = Batch{scissor_, current_texture_, clips_.index(), 0, 0};
    draw_calls_ = 0;
    in_frame_ = true;

    commands_->begin_render_pass(target, desc);
    commands_->bind_pipeline(*pipeline_);
    commands_->set_viewport(
        rhi::Viewport{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height)});
    commands_->set_scissor(scissor_);
    update_push_constants(clips_.index());
}

void Batched2DRenderer::end() {
    if (!in_frame_) {
        return;
    }
    if (pass_broken_) {
        // A frame that ends with the pass broken still closes its pass exactly once (resume_pass()
        // reopens it so end() has the same post-condition as the unbroken path).
        resume_pass();
    }
    flush();
    commands_->end_render_pass();
    commands_ = nullptr;
    frame_target_ = nullptr;
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
    update_push_constants(clips_.index());
}

void Batched2DRenderer::set_scissor(const rhi::Scissor& scissor) {
    if (!same_scissor(scissor_, scissor) || !same_scissor(current_batch_.scissor, scissor)) {
        flush_batch();
    }
    scissor_ = scissor;
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::push_scissor(const rhi::Scissor& scissor) {
    scissor_stack_.push_back(scissor_);
    const rhi::Scissor next = rhi::intersect(scissor_, scissor);
    if (!same_scissor(next, current_batch_.scissor)) {
        flush_batch();
    }
    scissor_ = next;
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::pop_scissor() {
    const rhi::Scissor next = scissor_stack_.empty() ? base_scissor_ : scissor_stack_.back();
    if (!scissor_stack_.empty()) {
        scissor_stack_.pop_back();
    }
    if (!same_scissor(next, current_batch_.scissor)) {
        flush_batch();
    }
    scissor_ = next;
    current_batch_.scissor = scissor_;
}

void Batched2DRenderer::push_clip(const clip::RoundedRect& input) {
    if (!in_frame_) {
        return;
    }
    // Clips are tested in target-pixel space (gl_FragCoord), so they take the same global content
    // scale as the positions they guard. Radii use the mean of the two axes: they are exact for the
    // uniform scale a render-target downgrade uses, and only an approximation otherwise.
    clip::RoundedRect scaled = input;
    if (content_scale_x_ != 1.0f || content_scale_y_ != 1.0f) {
        const float radius_scale = (content_scale_x_ + content_scale_y_) * 0.5f;
        scaled.rect.x *= content_scale_x_;
        scaled.rect.y *= content_scale_y_;
        scaled.rect.width *= content_scale_x_;
        scaled.rect.height *= content_scale_y_;
        scaled.top_left *= radius_scale;
        scaled.top_right *= radius_scale;
        scaled.bottom_right *= radius_scale;
        scaled.bottom_left *= radius_scale;
    }
    // The frame's own scissor (damage or scroll) is the base this clip intersects, so it is handed
    // to the table as the level's entry scissor and the pop later restores it exactly.
    apply_clip_state(clips_.push(clip::clamp_radii(scaled), scissor_));
}

void Batched2DRenderer::pop_clip() {
    if (!in_frame_ || clips_.empty()) {
        return;
    }
    apply_clip_state(clips_.pop(scissor_));
}

void Batched2DRenderer::set_transform(const rhi::Mat3& transform) {
    transform_ = transform;
}

void Batched2DRenderer::set_content_scale(float x, float y) {
    // A non-finite or non-positive factor would collapse or mirror the frame; ignore it and keep
    // the full-quality mapping so a bad caller cannot blank the window.
    if (!std::isfinite(x) || !std::isfinite(y) || x <= 0.0f || y <= 0.0f) {
        content_scale_x_ = 1.0f;
        content_scale_y_ = 1.0f;
        return;
    }
    content_scale_x_ = x;
    content_scale_y_ = y;
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

std::uint32_t Batched2DRenderer::vertex_count() const {
    return vertices_.vertex_count();
}

std::uint32_t Batched2DRenderer::pending_batches() const {
    return static_cast<std::uint32_t>(batches_.size() + (current_batch_.vertex_count > 0 ? 1 : 0));
}

void Batched2DRenderer::flush() {
    if (!in_frame_ || vertices_.staged_vertices() == 0) {
        return;
    }

    flush_batch();

    // Append after whatever earlier flushes of this frame wrote: their draws are recorded but not
    // executed yet, so overwriting offset 0 would change what they draw.
    const VertexArena::Span span = vertices_.upload();
    commands_->bind_vertex_buffer(*span.buffer);
    const std::uint32_t base = span.base_vertex;

    std::uint32_t emitted_draws = 0;
    if (damage_regions_.empty()) {
        for (const Batch& batch : batches_) {
            commands_->set_scissor(batch.scissor);
            update_push_constants(batch.clip_state);
            commands_->bind_texture(0, *batch.texture);
            commands_->draw(batch.vertex_count, base + batch.first_vertex);
            ++emitted_draws;
        }
    } else {
        // Replay each batch once per damage region. Regions are disjoint, so a pixel is never
        // blended twice even when several regions intersect the same batch.
        for (const Batch& batch : batches_) {
            bool state_bound = false;
            for (const rhi::Scissor& region : damage_regions_) {
                const rhi::Scissor clipped = rhi::intersect(batch.scissor, region);
                if (clipped.width == 0 || clipped.height == 0) {
                    continue;
                }
                if (!state_bound) {
                    update_push_constants(batch.clip_state);
                    commands_->bind_texture(0, *batch.texture);
                    state_bound = true;
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
    current_batch_ = Batch{scissor_, current_texture_, clips_.index(), 0, 0};
}

void Batched2DRenderer::flush_batch() {
    if (current_batch_.vertex_count == 0) {
        return;
    }

    batches_.push_back(current_batch_);
    current_batch_.first_vertex = vertices_.staged_vertices();
    current_batch_.vertex_count = 0;
    current_batch_.scissor = scissor_;
    current_batch_.texture = current_texture_;
    current_batch_.clip_state = clips_.index();
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
        px *= content_scale_x_;
        py *= content_scale_y_;

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
        vertices_.push_vertex(vertex);
    }

    current_batch_.vertex_count += 6;
}

void Batched2DRenderer::apply_clip_state(const ClipStateTable::Update& update) {
    const bool state_changed = update.state != current_batch_.clip_state;
    const bool scissor_changed = !same_scissor(update.scissor, current_batch_.scissor);
    if (state_changed || scissor_changed) {
        // Batching split rule: a draw batch covers one (texture, scissor, folded-clip-uniform)
        // tuple. Clip push/pop only costs a split when the folded uniform set or scissor changes.
        flush_batch();
    }
    scissor_ = update.scissor;
    current_batch_.scissor = update.scissor;
    current_batch_.clip_state = update.state;
}

void Batched2DRenderer::update_push_constants(std::uint32_t state_index) {
    Batched2DPushConstants push;
    const float aa_radius = (config_.enable_aa && !config_.golden_mode) ? 1.0f : 0.0f;
    clips_.pack_push_constants(state_index, extent_, aa_radius, push);
    commands_->push_constants(&push, sizeof(push));
}

void Batched2DRenderer::resume_pass_callback(void* context) {
    static_cast<Batched2DRenderer*>(context)->resume_pass();
}

} // namespace tessera::rendering::gpu
