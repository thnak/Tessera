// SPDX-License-Identifier: MIT
//
// The clip stack of the batched 2D renderer: rounded and shaped clips are pushed in paint
// coordinates, scaled to the target here, and handed to ClipStateTable. Split from
// batched2d_renderer.cpp to keep both inside the file-size budget.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"

namespace tessera::rendering::gpu {

void Batched2DRenderer::push_clip(const clip::RoundedRect& input) {
    if (!in_frame_) {
        return;
    }
    // Clips are tested in target-pixel space (gl_FragCoord), so they take the same global content
    // scale as the positions they guard. Radii use the mean of the two axes: they are exact for the
    // uniform scale a render-target downgrade uses, and only an approximation otherwise. The CPU
    // scissor is derived from this scaled rect by ClipStateTable::push (enclosing_scissor), so the
    // scissor fast path and the shader clip agree on the same target-space rectangle.
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

void Batched2DRenderer::push_clip_shape(const clip::ShapeClip& input) {
    if (!in_frame_) {
        return;
    }
    // Same target-space rule as push_clip(): the shape is tested at gl_FragCoord, so its box and
    // its vertices take the frame's content scale, and the scissor derives from the scaled box.
    clip::ShapeClip scaled = input;
    if (content_scale_x_ != 1.0f || content_scale_y_ != 1.0f) {
        scaled.record.x *= content_scale_x_;
        scaled.record.y *= content_scale_y_;
        scaled.record.width *= content_scale_x_;
        scaled.record.height *= content_scale_y_;
        const float radius_scale = (content_scale_x_ + content_scale_y_) * 0.5f;
        for (float& radius : scaled.record.radii) {
            radius *= radius_scale;
        }
        shape_scratch_.assign(input.vertices.begin(), input.vertices.end());
        for (clip::ClipVertex& vertex : shape_scratch_) {
            vertex.x *= content_scale_x_;
            vertex.y *= content_scale_y_;
        }
        scaled.vertices = shape_scratch_;
    }
    apply_clip_state(clips_.push_shape(scaled, scissor_));
}

void Batched2DRenderer::pop_clip() {
    if (!in_frame_ || clips_.empty()) {
        return;
    }
    apply_clip_state(clips_.pop(scissor_));
}

} // namespace tessera::rendering::gpu
