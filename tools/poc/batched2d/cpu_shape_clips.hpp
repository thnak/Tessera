// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/gpu/clip_shapes.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <cstdint>
#include <vector>

namespace tessera::poc::batched2d {

// The rounded clips of the CPU paint sink: the CPU twin of the renderer's clip set. Each rounded
// `push_clip` becomes a `RoundedRect` clip record (the radii clamped like the renderer clamps
// them), and a pixel's coverage is the product of every open record's coverage, which is what the
// fragment shader multiplies into `clip_alpha`. A clip with no rounded corner adds no record: its
// rectangle is already part of the sink's integer-free rect clip, exactly as the GPU takes the
// scissor fast path for it.
//
// `aa_radius` is the shader's: 0 is the hard pixel-centre test of golden mode (`inside_clip`), 0.5
// the half-pixel blend of an anti-aliased frame (`clip_coverage`). The coverage functions live in
// `gpu::clip` (clip_shapes.hpp) beside the hard rule they extend, so the CPU and the shader keep
// one definition of both.
class ShapeClips {
public:
    void set_aa_radius(float aa_radius) {
        aa_radius_ = aa_radius < 0.0f ? 0.0f : aa_radius;
    }
    [[nodiscard]] float aa_radius() const {
        return aa_radius_;
    }

    // True when no rounded clip is open: every coverage query is 1.
    [[nodiscard]] bool empty() const {
        return records_.empty();
    }

    void push(const ui::paint::RoundedClip& clip) {
        const bool rounded = clip.rounded();
        pushed_.push_back(rounded);
        if (rounded) {
            records_.push_back(record_of(clip));
        }
    }

    void pop() {
        if (pushed_.empty()) {
            return;
        }
        if (pushed_.back()) {
            records_.pop_back();
        }
        pushed_.pop_back();
    }

    // The coverage of the open clips at the pixel centre (x, y).
    [[nodiscard]] float coverage(float x, float y) const {
        float result = 1.0f;
        for (const rendering::gpu::clip::ClipRecord& record : records_) {
            result *= rendering::gpu::clip::rounded_rect_coverage(record, x, y, aa_radius_);
            if (result <= 0.0f) {
                return 0.0f;
            }
        }
        return result;
    }

    // The coverage of one more clip, a draw's own (a gradient's `clip`: the node's rounded border
    // box), at the pixel centre. A square clip covers everything its rect does.
    [[nodiscard]] float coverage_of(const ui::paint::RoundedClip& clip, float x, float y) const {
        if (!clip.rounded()) {
            return 1.0f;
        }
        return rendering::gpu::clip::rounded_rect_coverage(record_of(clip), x, y, aa_radius_);
    }

private:
    static rendering::gpu::clip::ClipRecord record_of(const ui::paint::RoundedClip& clip) {
        const rendering::gpu::clip::RoundedRect clamped = rendering::gpu::clip::make_rounded_rect(
            clip.rect, clip.top_left, clip.top_right, clip.bottom_right, clip.bottom_left);
        rendering::gpu::clip::ClipRecord record;
        record.kind = static_cast<std::uint32_t>(rendering::gpu::clip::RecordKind::RoundedRect);
        record.x = clamped.rect.x;
        record.y = clamped.rect.y;
        record.width = clamped.rect.width;
        record.height = clamped.rect.height;
        record.radii[0] = clamped.top_left;
        record.radii[1] = clamped.top_right;
        record.radii[2] = clamped.bottom_right;
        record.radii[3] = clamped.bottom_left;
        return record;
    }

    std::vector<rendering::gpu::clip::ClipRecord> records_;
    std::vector<bool> pushed_;
    float aa_radius_ = 0.0f;
};

} // namespace tessera::poc::batched2d
