// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

// The texture-array half of the batched renderer (RHI 1.7, issue #480 part 2), split out of
// batched2d_renderer.cpp so that file stays inside its size budget - the same reason
// batched2d_lcd.cpp exists.
//
// The batch key is (texture, scissor, folded clip state), so a texture change ends the batch. On a
// device with descriptor indexing the frame's textures are one array instead and each vertex
// carries its slot, so interleaved images and glyph pages share one draw.
namespace tessera::rendering::gpu {

// Why a batch ended, counted per frame (issue #480 part 2). The texture share is what descriptor
// indexing removes; the rest are structural, so a frame's draw-call count is attributable.
void Batched2DRenderer::count_batch_split(BatchSplitReason reason) {
    switch (reason) {
    case BatchSplitReason::Texture:
        ++split_stats_.texture;
        break;
    case BatchSplitReason::Pipeline:
        ++split_stats_.pipeline;
        break;
    case BatchSplitReason::Scissor:
        ++split_stats_.scissor;
        break;
    case BatchSplitReason::Clip:
        ++split_stats_.clip;
        break;
    case BatchSplitReason::Flush:
        ++split_stats_.flush;
        break;
    }
}

void Batched2DRenderer::set_texture_array_pipeline(rhi::Pipeline* pipeline) {
    // A device without the capability cannot honour the request, and only a pipeline built for the
    // array layout can bind one. Refuse instead of silently keeping the old path.
    if (pipeline != nullptr && device_->texture_array_capacity() == 0) {
        throw std::invalid_argument("set_texture_array_pipeline: this device has no texture array");
    }
    array_pipeline_ = pipeline;
}

// The batch key is (texture, scissor, folded clip state). On the array path a texture change is not
// a split: the texture joins the frame's array and the vertex carries its slot, so interleaved
// images and glyph pages share one draw. A pipeline change always splits, because it changes the
// bound pipeline and the descriptor layout.
std::uint32_t Batched2DRenderer::begin_quad_batch(rhi::Texture* texture) {
    std::uint32_t texture_index = 0;
    if (quad_pipeline_ != current_batch_.pipeline) {
        flush_batch(BatchSplitReason::Pipeline);
        current_batch_.pipeline = quad_pipeline_;
    }
    if (array_pipeline_ != nullptr && frame_texture_slot(texture, texture_index)) {
        current_batch_.texture_array = true;
    } else if (texture != current_texture_) {
        // The one-set-per-texture path, or a frame that outgrew the device's array.
        flush_batch(BatchSplitReason::Texture);
        current_texture_ = texture;
        current_batch_.texture = texture;
        current_batch_.texture_array = false;
    }
    return texture_index;
}

// The frame's array slot for `texture`, adding it on first use. False when the frame already holds
// `texture_array_capacity()` textures: the caller then keeps the one-set-per-texture path for this
// quad, which is the documented bounded behavior (the array length is the device's, not the
// scene's).
bool Batched2DRenderer::frame_texture_slot(rhi::Texture* texture, std::uint32_t& slot) {
    const auto found = std::find(frame_textures_.begin(), frame_textures_.end(), texture);
    if (found != frame_textures_.end()) {
        slot = static_cast<std::uint32_t>(found - frame_textures_.begin());
        return true;
    }
    if (frame_textures_.size() >= device_->texture_array_capacity()) {
        return false;
    }
    slot = static_cast<std::uint32_t>(frame_textures_.size());
    frame_textures_.push_back(texture);
    return true;
}

// Binds the batch's sampled images: the frame's array on the descriptor-indexing path, one texture
// on the one-set-per-texture path. The array is bound once per flush, not per batch: a descriptor
// set is read when the GPU executes the draw, so rewriting one set per batch would make every draw
// read the last write.
void Batched2DRenderer::bind_batch_textures(const Batch& batch) {
    if (!batch.texture_array) {
        commands_->bind_texture(0, *batch.texture);
        return;
    }
    std::vector<const rhi::Texture*> textures(frame_textures_.begin(), frame_textures_.end());
    commands_->bind_texture_array(0, textures);
}

} // namespace tessera::rendering::gpu
