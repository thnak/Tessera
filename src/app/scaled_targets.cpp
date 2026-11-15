// SPDX-License-Identifier: MIT

#include "scaled_targets.hpp"

#include <exception>

namespace tessera::app::detail {
namespace {

// The clear the target starts each frame with. Built here rather than taken from
// window_internal.hpp so this policy file does not pull the Vulkan presentation headers into a
// translation unit that only ever calls Device::create_render_target.
rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format,
                                  rhi::Color clear_color) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = clear_color;
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
                                         rhi::Color clear_color, bool& fallback) {
    fallback = false;
    if (slot >= targets_.size() || extent.width == 0 || extent.height == 0) {
        return nullptr;
    }
    Entry& entry = targets_[slot];
    // A held downgrade at the same extent costs no allocation: the image is reused for as long as
    // the decided extent lasts. An upgrade to full quality stops using it, and the next downgrade
    // at the same extent reuses it again.
    if (entry.target != nullptr && entry.extent.width == extent.width &&
        entry.extent.height == extent.height && entry.target->format() == format) {
        return entry.target.get();
    }
    try {
        entry.target = device.create_render_target(target_desc(extent, format, clear_color));
        entry.extent = extent;
        if (entry.target == nullptr) {
            entry.extent = {};
            fallback = true;
        }
        return entry.target.get();
    } catch (const std::exception&) {
        // A backend refusal is a value, not a failure: the frame presents at full quality instead,
        // and the caller counts the fallback.
        entry.target.reset();
        entry.extent = {};
        fallback = true;
        return nullptr;
    }
}

} // namespace tessera::app::detail
