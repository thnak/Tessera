// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/offscreen_target_pool.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace tessera::rendering::gpu {

OffscreenTargetPool::OffscreenTargetPool(rhi::Device& device, OffscreenTargetPoolConfig config)
    : device_(&device),
      config_(config) {}

OffscreenTargetPool::~OffscreenTargetPool() = default;

void OffscreenTargetPool::begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                                      rhi::TextureFormat format) {
    frame_index_ = frame_index;
    frames_in_flight_ = std::max(frames_in_flight, 1u);
    format_ = format;
    for (Entry& entry : entries_) {
        entry.used_this_frame = false;
    }
}

bool OffscreenTargetPool::free_for_next_frame(const Entry& entry) const {
    if (entry.target == nullptr || entry.last_used_frame == 0 || entry.used_this_frame) {
        return false;
    }
    if (frame_index_ < entry.last_used_frame) {
        // Non-monotonic frame index from a caller bug: stay conservative rather than recycle a
        // target whose frame may still be reading it.
        return false;
    }
    return frame_index_ - entry.last_used_frame >= frames_in_flight_;
}

rhi::RenderTarget* OffscreenTargetPool::acquire(rhi::Extent2D extent) {
    if (config_.max_targets == 0 || extent.width == 0 || extent.height == 0) {
        return nullptr;
    }

    // Prefer a free target that already fits; otherwise grow the first free one. A target used by
    // this frame is never a candidate: a copy recorded against it may still be in the command list.
    Entry* fitting = nullptr;
    Entry* growable = nullptr;
    for (Entry& entry : entries_) {
        if (!free_for_next_frame(entry)) {
            continue;
        }
        const rhi::Extent2D have = entry.target->extent();
        if (entry.target->format() == format_ && have.width >= extent.width &&
            have.height >= extent.height) {
            fitting = &entry;
            break;
        }
        if (growable == nullptr) {
            growable = &entry;
        }
    }

    Entry* const chosen = fitting != nullptr ? fitting : growable;
    if (chosen != nullptr) {
        const rhi::Extent2D have = chosen->target->extent();
        const bool replace = chosen->target->format() != format_ || have.width < extent.width ||
                             have.height < extent.height;
        if (replace) {
            // Free, so its frame completed: recreating it here cannot invalidate a recorded copy.
            chosen->target = create_target(extent);
            ++created_;
        } else {
            ++reused_;
        }
        chosen->last_used_frame = frame_index_;
        chosen->used_this_frame = true;
        return chosen->target.get();
    }

    if (entries_.size() >= config_.max_targets) {
        return nullptr;
    }
    Entry entry;
    entry.target = create_target(extent);
    entry.last_used_frame = frame_index_;
    entry.used_this_frame = true;
    ++created_;
    entries_.push_back(std::move(entry));
    return entries_.back().target.get();
}

rhi::Extent2D OffscreenTargetPool::target_extent(std::uint32_t index) const {
    if (index >= entries_.size()) {
        return {};
    }
    return entries_[index].target->extent();
}

std::unique_ptr<rhi::RenderTarget> OffscreenTargetPool::create_target(rhi::Extent2D extent) const {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format_;
    // The pool owns no pixels until a caller writes them; a clear keeps the first read of a
    // partially copied target defined (tests and snapshot paths read back the whole target).
    desc.clear_color = {0.0f, 0.0f, 0.0f, 0.0f};
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    return device_->create_render_target(desc);
}

} // namespace tessera::rendering::gpu
