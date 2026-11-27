// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/offscreen_target_pool.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace tessera::rendering::gpu {
namespace {

// glassMaxBlurTargets is validated to 0..16; anything above is a programming error, not a reason
// to reserve gigabytes.
constexpr std::uint32_t kReserveLimit = 64;

} // namespace

OffscreenTargetPool::OffscreenTargetPool(rhi::Device& device, OffscreenTargetPoolConfig config)
    : device_(&device),
      config_(config) {
    // The cap is small and fixed, so reserving it once keeps entries_ from ever allocating on the
    // frame path (alloc_per_frame): a target is created lazily, its slot is not.
    entries_.reserve(std::min(config_.max_targets, kReserveLimit));
    // Regrown targets and evicted retained layers both wait here for their frames to rotate.
    retired_.reserve(kReserveLimit);
}

OffscreenTargetPool::~OffscreenTargetPool() = default;

void OffscreenTargetPool::begin_frame(std::uint64_t frame_index, std::uint32_t frames_in_flight,
                                      rhi::TextureFormat format) {
    frame_index_ = frame_index;
    frames_in_flight_ = std::max(frames_in_flight, 1u);
    format_ = format;
    for (Entry& entry : entries_) {
        entry.used_this_frame = false;
    }
    stats_.live = 0;
    stats_.peak_live = 0;
    stats_.exhausted = 0;

    drop_retired(false);
    if (config_.idle_trim_frames == 0) {
        return;
    }
    // Never sooner than the frames-in-flight window: the GPU may still read a target of those.
    const std::uint64_t idle = std::max(config_.idle_trim_frames, frames_in_flight_);
    for (std::size_t index = entries_.size(); index-- > 0;) {
        const Entry& entry = entries_[index];
        if (available(entry) && rotated(entry.last_used_frame) &&
            frame_index_ - entry.last_used_frame >= idle) {
            erase_entry(index);
        }
    }
}

bool OffscreenTargetPool::rotated(std::uint64_t last_used_frame) const {
    if (last_used_frame == 0) {
        return false;
    }
    if (frame_index_ < last_used_frame) {
        // Non-monotonic frame index from a caller bug: stay conservative rather than treat a frame
        // that may still be reading the target as complete.
        return false;
    }
    return frame_index_ - last_used_frame >= frames_in_flight_;
}

bool OffscreenTargetPool::available(const Entry& entry) const {
    return entry.target != nullptr && (!entry.held || rotated(entry.last_used_frame));
}

std::uint64_t OffscreenTargetPool::extent_bytes(rhi::Extent2D extent) const {
    // Every pooled target is single-sampled; the one-byte format is the only non-4-byte one.
    const std::uint64_t texel = format_ == rhi::TextureFormat::R8Unorm ? 1u : 4u;
    return static_cast<std::uint64_t>(extent.width) * extent.height * texel;
}

void OffscreenTargetPool::note_live(Entry& entry) {
    entry.held = true;
    entry.used_this_frame = true;
    entry.last_used_frame = frame_index_;
    ++stats_.live;
    stats_.peak_live = std::max(stats_.peak_live, stats_.live);
    stats_.peak_live_max = std::max(stats_.peak_live_max, stats_.live);
}

void OffscreenTargetPool::erase_entry(std::size_t index) {
    bytes_ -= std::min(bytes_, entries_[index].bytes);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(index));
    ++stats_.trimmed_total;
}

void OffscreenTargetPool::drop_retired(bool all) {
    for (std::size_t index = retired_.size(); index-- > 0;) {
        if (all || rotated(retired_[index].last_used_frame)) {
            bytes_ -= std::min(bytes_, retired_[index].bytes);
            retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }
}

rhi::RenderTarget* OffscreenTargetPool::acquire(rhi::Extent2D extent) {
    if (config_.max_targets == 0 || extent.width == 0 || extent.height == 0) {
        return nullptr;
    }

    // Prefer an available target that already fits; otherwise regrow the first available one.
    Entry* fitting = nullptr;
    Entry* growable = nullptr;
    for (Entry& entry : entries_) {
        if (!available(entry)) {
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

    if (fitting != nullptr) {
        ++reused_;
        note_live(*fitting);
        return fitting->target.get();
    }

    if (growable != nullptr) {
        // The new extent is the per-axis maximum, so the target keeps its high-water size.
        const rhi::Extent2D have = growable->target->extent();
        const rhi::Extent2D grown{std::max(have.width, extent.width),
                                  std::max(have.height, extent.height)};
        const std::uint64_t wanted = extent_bytes(grown);
        // The old image survives in retired_ while a frame in flight may still read it; its bytes
        // stay charged, so the cap holds for the memory the GPU really keeps.
        const bool retire = !rotated(growable->last_used_frame);
        const std::uint64_t after = retire ? bytes_ + wanted : bytes_ - growable->bytes + wanted;
        const bool room = !retire || retired_.size() < retired_.capacity();
        if (room && (config_.max_bytes == 0 || after <= config_.max_bytes)) {
            if (retire) {
                retired_.push_back(Retired{std::move(growable->target), growable->last_used_frame,
                                           growable->bytes});
            }
            bytes_ = after;
            growable->target = create_target(grown);
            growable->bytes = wanted;
            ++created_;
            stats_.peak_bytes = std::max(stats_.peak_bytes, bytes_);
            note_live(*growable);
            return growable->target.get();
        }
    } else if (entries_.size() < config_.max_targets) {
        const std::uint64_t wanted = extent_bytes(extent);
        if (config_.max_bytes == 0 || bytes_ + wanted <= config_.max_bytes) {
            Entry entry;
            entry.target = create_target(extent);
            entry.bytes = wanted;
            bytes_ += wanted;
            stats_.peak_bytes = std::max(stats_.peak_bytes, bytes_);
            ++created_;
            entries_.push_back(std::move(entry));
            note_live(entries_.back());
            return entries_.back().target.get();
        }
    }

    ++stats_.exhausted;
    ++stats_.exhausted_total;
    return nullptr;
}

void OffscreenTargetPool::release(const rhi::RenderTarget* target) {
    if (target == nullptr) {
        return;
    }
    for (Entry& entry : entries_) {
        if (entry.target.get() == target) {
            if (entry.held) {
                entry.held = false;
                if (entry.used_this_frame && stats_.live > 0) {
                    --stats_.live;
                }
                entry.used_this_frame = false;
            }
            return;
        }
    }
}

void OffscreenTargetPool::trim(bool all_complete) {
    if (all_complete) {
        while (!entries_.empty()) {
            erase_entry(entries_.size() - 1);
        }
        drop_retired(true);
        return;
    }
    drop_retired(false);
    for (std::size_t index = entries_.size(); index-- > 0;) {
        if (available(entries_[index]) && rotated(entries_[index].last_used_frame)) {
            erase_entry(index);
        }
    }
}

bool OffscreenTargetPool::charge(std::uint64_t bytes) {
    if (config_.max_bytes != 0 && bytes_ + bytes > config_.max_bytes) {
        return false;
    }
    bytes_ += bytes;
    stats_.peak_bytes = std::max(stats_.peak_bytes, bytes_);
    return true;
}

void OffscreenTargetPool::refund(std::uint64_t bytes) {
    bytes_ -= std::min(bytes_, bytes);
}

void OffscreenTargetPool::retire(std::unique_ptr<rhi::RenderTarget> target,
                                 std::uint64_t last_used_frame, std::uint64_t bytes) {
    if (target == nullptr) {
        return;
    }
    // A target no frame ever used has no reader to wait for.
    if (last_used_frame == 0 || rotated(last_used_frame)) {
        refund(bytes);
        return;
    }
    retired_.push_back(Retired{std::move(target), last_used_frame, bytes});
}

OffscreenPoolStats OffscreenTargetPool::stats() const {
    OffscreenPoolStats out = stats_;
    out.targets = static_cast<std::uint32_t>(entries_.size());
    out.bytes = bytes_;
    out.max_targets = config_.max_targets;
    out.max_bytes = config_.max_bytes;
    return out;
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
