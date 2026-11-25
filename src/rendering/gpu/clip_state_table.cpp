// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/clip_state_table.hpp"

#include <algorithm>
#include <cstring>

namespace tessera::rendering::gpu {
namespace {

// One rounded rectangle as a record. Slice 3 emits `RoundedRect` only, with the four circular
// corner radii in `radii[0..3]`; the elliptical second components stay zero until slice 4.
clip::ClipRecord to_record(const clip::RoundedRect& source) {
    clip::ClipRecord record;
    record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
    record.x = source.rect.x;
    record.y = source.rect.y;
    record.width = source.rect.width;
    record.height = source.rect.height;
    record.radii[0] = source.top_left;
    record.radii[1] = source.top_right;
    record.radii[2] = source.bottom_right;
    record.radii[3] = source.bottom_left;
    return record;
}

// True when two records describe the same clip. A polygon's vertices are pooled and deduplicated,
// so equal polygons have equal `first_vertex` and the run index stands for their contents.
bool same_record(const clip::ClipRecord& a, const clip::ClipRecord& b) {
    if (a.kind != b.kind || a.flags != b.flags || a.first_vertex != b.first_vertex ||
        a.vertex_count != b.vertex_count || a.x != b.x || a.y != b.y || a.width != b.width ||
        a.height != b.height) {
        return false;
    }
    for (int index = 0; index < 8; ++index) {
        if (a.radii[index] != b.radii[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

ClipStateTable::ClipStateTable() {
    // Set 0 is the empty clip set, so index 0 always means "no shader clips" and an unclipped batch
    // needs no special case in the renderer.
    sets_.reserve(16);
    sets_.emplace_back();
    clip_stack_.reserve(clip::kMaxClipDepth);
    clip_folded_.reserve(clip::kMaxClipDepth);
    scissor_stack_.reserve(clip::kMaxClipDepth);
}

void ClipStateTable::reset() {
    clip_stack_.clear();
    clip_folded_.clear();
    scissor_stack_.clear();
    sets_.resize(1);
    vertices_.clear();
    pooled_.clear();
    shape_fallbacks_ = 0;
    flat_dirty_ = true;
    current_set_ = 0;
}

ClipStateTable::Update ClipStateTable::push(const clip::RoundedRect& clip, rhi::Scissor current) {
    Entry entry;
    entry.bounds = clip;
    return push_entry(entry, current);
}

std::int64_t ClipStateTable::pool_vertices(std::span<const clip::ClipVertex> vertices) {
    for (const PooledRun& run : pooled_) {
        if (run.count == vertices.size() &&
            std::memcmp(vertices_.data() + run.first, vertices.data(),
                        vertices.size() * sizeof(clip::ClipVertex)) == 0) {
            return run.first;
        }
    }
    if (vertices_.size() + vertices.size() > clip::kMaxClipVertices) {
        return -1;
    }
    const auto first = static_cast<std::uint32_t>(vertices_.size());
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
    pooled_.push_back(PooledRun{first, static_cast<std::uint32_t>(vertices.size())});
    return first;
}

ClipStateTable::Update ClipStateTable::push_shape(const clip::ShapeClip& shape,
                                                  rhi::Scissor current) {
    Entry entry;
    entry.shape = shape.record;
    entry.bounds.rect =
        rhi::Rect{shape.record.x, shape.record.y, shape.record.width, shape.record.height};
    entry.shaped = true;
    if (shape.record.kind == static_cast<std::uint32_t>(clip::RecordKind::Polygon)) {
        entry.shape.first_vertex = 0;
        entry.shape.vertex_count = 0;
        const std::size_t count = shape.vertices.size();
        if (count < 3) {
            // No area: an empty rectangle is what clips everything away.
            entry.shape.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
            entry.shape.width = 0.0f;
            entry.shape.height = 0.0f;
            entry.bounds.rect.width = 0.0f;
            entry.bounds.rect.height = 0.0f;
        } else {
            const std::int64_t first =
                count <= clip::kMaxPolygonVertices ? pool_vertices(shape.vertices) : -1;
            if (first < 0) {
                // Over a limit: the bounding box, counted so the gap is visible in diagnostics.
                entry.shaped = false;
                ++shape_fallbacks_;
            } else {
                entry.shape.first_vertex = static_cast<std::uint32_t>(first);
                entry.shape.vertex_count = static_cast<std::uint32_t>(count);
            }
        }
    }
    return push_entry(entry, current);
}

ClipStateTable::Update ClipStateTable::push_entry(const Entry& entry, rhi::Scissor current) {
    const clip::RoundedRect& clip = entry.bounds;
    // The enclosing scissor is recorded per level: the renderer may have a damage or scroll scissor
    // narrower than the clip, and a pop must restore whatever was in effect rather than the
    // frame's full-target rect.
    scissor_stack_.push_back(current);
    if (clip_stack_.size() < clip::kMaxClipDepth) {
        clip_stack_.push_back(entry);
        clip_folded_.push_back(false);
    } else {
        // Documented nesting bound: deeper clips are folded into the innermost active clip
        // (conservative intersection) instead of growing the stack. The fold keeps bounds only, so
        // a shaped clip at this depth reduces to its bounding box.
        clip_stack_.back().bounds = clip::intersect_clips(clip_stack_.back().bounds, clip);
        clip_stack_.back().shaped = false;
        clip_folded_.push_back(true);
    }
    rebuild();
    return Update{current_set_, rhi::intersect(current, clip::enclosing_scissor(clip))};
}

ClipStateTable::Update ClipStateTable::pop(rhi::Scissor current) {
    if (clip_folded_.empty()) {
        // Unbalanced pop: nothing to restore, so the caller's scissor stands.
        return Update{current_set_, current};
    }
    const bool folded = clip_folded_.back();
    clip_folded_.pop_back();
    if (!folded && !clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
    // The recorded scissor already accounted for every enclosing clip, so restoring it is exact.
    const rhi::Scissor restored = scissor_stack_.empty() ? current : scissor_stack_.back();
    if (!scissor_stack_.empty()) {
        scissor_stack_.pop_back();
    }
    rebuild();
    return Update{current_set_, restored};
}

void ClipStateTable::rebuild() {
    // One record per active clip, in stack order, which is the order the shader multiplies coverage
    // in. A stack deeper than the buffer's capacity folds the overflow into the last record
    // conservatively, the same rule the stack uses at its own bound; the CPU-side scissor still
    // reflects every clip rect, so the result never paints outside any active clip.
    std::vector<clip::ClipRecord> records;
    records.reserve(std::min<std::size_t>(clip_stack_.size(), clip::kMaxClipRecords));
    for (const Entry& active : clip_stack_) {
        if (records.size() >= clip::kMaxClipRecords) {
            records.back() = to_record(
                clip::intersect_clips(clip_stack_[records.size() - 1].bounds, active.bounds));
            continue;
        }
        records.push_back(active.shaped ? active.shape : to_record(active.bounds));
    }
    current_set_ = intern(records);
    flat_dirty_ = true;
}

std::uint32_t ClipStateTable::intern(const std::vector<clip::ClipRecord>& records) {
    for (std::uint32_t index = 0; index < sets_.size(); ++index) {
        const std::vector<clip::ClipRecord>& candidate = sets_[index];
        if (candidate.size() != records.size()) {
            continue;
        }
        bool equal = true;
        for (std::size_t offset = 0; offset < records.size(); ++offset) {
            if (!same_record(candidate[offset], records[offset])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return index;
        }
    }
    sets_.push_back(records);
    return static_cast<std::uint32_t>(sets_.size() - 1);
}

std::span<const clip::ClipRecord> ClipStateTable::set(std::uint32_t index) const {
    if (index >= sets_.size()) {
        return {};
    }
    return std::span<const clip::ClipRecord>(sets_[index].data(), sets_[index].size());
}

const std::vector<clip::ClipRecord>& ClipStateTable::flat_records() const {
    if (flat_dirty_) {
        flatten();
    }
    return flat_records_;
}

const std::vector<clip::ClipSetRange>& ClipStateTable::set_ranges() const {
    if (flat_dirty_) {
        flatten();
    }
    return set_ranges_;
}

void ClipStateTable::flatten() const {
    // Every set's records concatenated in set order, so a set's range is a contiguous slice. A set
    // that would overflow the buffer's record capacity is truncated rather than written past it:
    // the renderer's scissor still bounds every draw, so dropping a set's shader refinement can
    // only paint inside the scissor, never outside it.
    flat_records_.clear();
    set_ranges_.clear();
    const std::uint32_t set_count =
        std::min<std::uint32_t>(static_cast<std::uint32_t>(sets_.size()), clip::kMaxClipSets);
    set_ranges_.reserve(set_count);
    for (std::uint32_t index = 0; index < set_count; ++index) {
        const std::vector<clip::ClipRecord>& records = sets_[index];
        const std::uint32_t room = std::min<std::uint32_t>(
            static_cast<std::uint32_t>(records.size()),
            clip::kMaxClipRecords - static_cast<std::uint32_t>(flat_records_.size()));
        clip::ClipSetRange range;
        range.first_record = static_cast<std::uint32_t>(flat_records_.size());
        range.count = room;
        flat_records_.insert(flat_records_.end(), records.begin(),
                             records.begin() + static_cast<std::ptrdiff_t>(room));
        set_ranges_.push_back(range);
    }
    flat_dirty_ = false;
}

void ClipStateTable::pack_push_constants(std::uint32_t index, rhi::Extent2D viewport,
                                         float aa_radius, Batched2DPushConstants& out) const {
    out.viewport_width = static_cast<float>(viewport.width);
    out.viewport_height = static_cast<float>(viewport.height);
    out.aa_radius = aa_radius;
    // The shader reads the records from the buffer; the block carries only which set to read.
    out.clip_set = index;
}

} // namespace tessera::rendering::gpu
