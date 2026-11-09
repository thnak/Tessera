// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::rendering::gpu {

struct Batched2DConfig {
    std::uint32_t initial_vertex_capacity = 4096;
    bool enable_aa = true;
    bool golden_mode = false;

    // Number of frames whose vertex data may be alive on the GPU at once. Each frame writes into
    // its own buffer slot, used round-robin. With the default of 1 the caller finishes a frame
    // (blocking submit() or waiting on the list) before beginning the next. With N > 1 the caller
    // must not begin frame k + N until frame k's command list is complete, because that frame
    // reuses frame k's slot.
    std::uint32_t frames_in_flight = 1;
};

class Batched2DRenderer {
public:
    Batched2DRenderer(rhi::Device& device, rhi::Pipeline& pipeline, rhi::Texture& white_texture,
                      Batched2DConfig config = {});
    ~Batched2DRenderer();

    Batched2DRenderer(const Batched2DRenderer&) = delete;
    Batched2DRenderer& operator=(const Batched2DRenderer&) = delete;

    void begin(rhi::CommandList& commands, const rhi::RenderTarget& target,
               const rhi::RenderTargetDesc& desc);
    // Damage-aware entry point. A full-redraw plan is identical to begin(); a partial
    // plan clips every emitted draw to exactly one damage region (see redraw_plan.hpp).
    // An empty partial plan is a no-op: no render pass is opened and nothing is drawn.
    // `plan.scissors` must be disjoint; DamageAccumulator guarantees that.
    void begin_damaged(rhi::CommandList& commands, const rhi::RenderTarget& target,
                       const rhi::RenderTargetDesc& desc, const RedrawPlan& plan);
    void end();

    void set_viewport(rhi::Extent2D extent);
    void set_scissor(const rhi::Scissor& scissor);
    void push_scissor(const rhi::Scissor& scissor);
    void pop_scissor();

    void set_transform(const rhi::Mat3& transform);
    void push_transform(const rhi::Mat3& transform);
    void pop_transform();

    void push_rect(const rhi::Rect& rect, rhi::Color color);
    void push_rounded_rect(const rhi::Rect& rect, rhi::Color color, float radius);
    void push_texture(const rhi::Rect& rect, rhi::Texture& texture, const rhi::Rect& uv,
                      rhi::Color color);

    void flush();

    [[nodiscard]] std::uint32_t draw_calls() const {
        return draw_calls_;
    }
    [[nodiscard]] std::uint32_t vertex_count() const {
        return emitted_vertices_;
    }
    [[nodiscard]] std::uint32_t pending_batches() const {
        return static_cast<std::uint32_t>(batches_.size() +
                                          (current_batch_.vertex_count > 0 ? 1 : 0));
    }

private:
    struct Batch {
        rhi::Scissor scissor;
        rhi::Texture* texture = nullptr;
        std::uint32_t first_vertex = 0;
        std::uint32_t vertex_count = 0;
    };

    void begin_frame(rhi::CommandList& commands, const rhi::RenderTarget& target,
                     const rhi::RenderTargetDesc& desc,
                     const std::vector<rhi::Scissor>* damage_regions);
    void select_next_slot();
    void ensure_buffer_space(std::size_t vertices);
    [[nodiscard]] rhi::Buffer& current_buffer();
    void flush_batch();
    void emit_quad(const rhi::Rect& rect, const rhi::Rect& uv, rhi::Color color, float radius,
                   float mode, rhi::Texture* texture);
    void update_push_constants();

    rhi::Device* device_ = nullptr;
    rhi::Pipeline* pipeline_ = nullptr;
    rhi::Texture* white_texture_ = nullptr;
    rhi::CommandList* commands_ = nullptr;

    Batched2DConfig config_;
    rhi::Extent2D extent_;
    rhi::Scissor base_scissor_;
    rhi::Scissor scissor_;
    std::vector<rhi::Scissor> scissor_stack_;
    std::vector<rhi::Scissor> damage_regions_;

    rhi::Mat3 transform_;
    std::vector<rhi::Mat3> transform_stack_;

    // Vertex memory for one in-flight frame. A frame normally uses a single buffer; when it runs
    // out of room it continues in a larger one, because draws already recorded this frame still
    // reference the earlier buffers. Extra buffers are dropped when the slot is reused.
    struct FrameSlot {
        std::vector<std::unique_ptr<rhi::Buffer>> buffers;
        std::uint32_t capacity = 0; // vertices in buffers.back()
    };
    std::vector<FrameSlot> slots_;
    std::uint32_t slot_index_ = 0;
    std::uint32_t buffer_used_ = 0; // vertices already written to the current buffer this frame
    std::vector<Vertex2D> vertices_;
    std::vector<Batch> batches_;
    Batch current_batch_;
    rhi::Texture* current_texture_ = nullptr;

    std::uint32_t draw_calls_ = 0;
    std::uint32_t emitted_vertices_ = 0;
    bool in_frame_ = false;
};

} // namespace tessera::rendering::gpu
