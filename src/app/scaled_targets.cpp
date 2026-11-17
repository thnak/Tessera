// SPDX-License-Identifier: MIT

#include "scaled_targets.hpp"

#include <exception>

namespace tessera::app::detail {
namespace {

// The clear the target starts each frame with. Built here rather than taken from
// window_internal.hpp so this policy file does not pull the Vulkan presentation headers into a
// translation unit that only ever calls Device::create_render_target.
rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format,
                                  rhi::Color clear_color, std::uint32_t sample_count) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = clear_color;
    desc.sample_count = sample_count;
    return desc;
}

} // namespace

void ScaledTargets::reset(bool resample_supported) {
    resample_supported_ = resample_supported;
    release();
}

void ScaledTargets::release() {
    for (Entry& entry : targets_) {
        entry.target.reset();
        entry.extent = {};
        entry.sample_count = 1;
    }
}

rhi::RenderTarget* ScaledTargets::get(std::size_t slot) const {
    return slot < targets_.size() ? targets_[slot].target.get() : nullptr;
}

rhi::Extent2D ScaledTargets::extent(std::size_t slot) const {
    return slot < targets_.size() ? targets_[slot].extent : rhi::Extent2D{};
}

rhi::RenderTarget* ScaledTargets::ensure(rhi::Device& device, std::size_t slot,
                                         rhi::Extent2D extent, rhi::TextureFormat format,
                                         rhi::Color clear_color, std::uint32_t sample_count,
                                         bool& fallback) {
    fallback = false;
    if (slot >= targets_.size() || extent.width == 0 || extent.height == 0) {
        return nullptr;
    }
    Entry& entry = targets_[slot];
    // A held downgrade at the same extent costs no allocation: the image is reused for as long as
    // the decided extent lasts. An upgrade to full quality stops using it, and the next downgrade
    // at the same extent reuses it again. The sample count is part of the identity because a 4x
    // target's multisample/resolve images cannot stand in for a 1-sample one (S8).
    if (entry.target != nullptr && entry.extent.width == extent.width &&
        entry.extent.height == extent.height && entry.target->format() == format &&
        entry.target->sample_count() == sample_count) {
        return entry.target.get();
    }
    try {
        entry.target =
            device.create_render_target(target_desc(extent, format, clear_color, sample_count));
        entry.extent = extent;
        entry.sample_count = sample_count;
        if (entry.target == nullptr) {
            entry.extent = {};
            entry.sample_count = 1;
            fallback = true;
        }
        return entry.target.get();
    } catch (const std::exception&) {
        // A backend refusal is a value, not a failure: the frame presents at full quality instead,
        // and the caller counts the fallback.
        entry.target.reset();
        entry.extent = {};
        entry.sample_count = 1;
        fallback = true;
        return nullptr;
    }
}

} // namespace tessera::app::detail
