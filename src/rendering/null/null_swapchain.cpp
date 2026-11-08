// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_swapchain.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace tessera::rendering::null {

NullSwapchain::NullSwapchain(const rhi::SwapchainDesc& desc)
    : desc_(desc),
      pending_extent_(desc.extent),
      acquired_(desc.image_count, std::uint8_t{0}) {
    if (desc.image_count < 2) {
        throw std::invalid_argument("swapchain image_count must be at least 2");
    }
}

rhi::Extent2D NullSwapchain::extent() const {
    return desc_.extent;
}

rhi::TextureFormat NullSwapchain::format() const {
    return desc_.format;
}

std::uint32_t NullSwapchain::image_count() const {
    return desc_.image_count;
}

std::expected<std::uint32_t, rhi::SwapchainError>
NullSwapchain::acquire_next_image(std::uint64_t /*timeout_ms*/) {
    // Resize reports out-of-date once and adopts the pending extent in that same call. The null
    // backend has no in-flight GPU work, so a resize retires every acquired image.
    if (out_of_date_) {
        out_of_date_ = false;
        desc_.extent = pending_extent_;
        next_image_ = 0;
        std::fill(acquired_.begin(), acquired_.end(), std::uint8_t{0});
        ++stats_.out_of_date;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }

    if (acquire_timeout_) {
        ++stats_.timeouts;
        return std::unexpected(rhi::SwapchainError::Timeout);
    }

    const std::uint32_t image = next_image_;
    next_image_ = (next_image_ + 1) % desc_.image_count;
    acquired_[image] = 1;
    ++stats_.acquires;
    return image;
}

std::expected<void, rhi::SwapchainError> NullSwapchain::present(std::uint32_t image) {
    if (image >= desc_.image_count || acquired_[image] == 0) {
        ++stats_.invalid_presents;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }

    acquired_[image] = 0;
    ++stats_.presents;
    return {};
}

void NullSwapchain::resize(rhi::Extent2D extent) {
    if (!out_of_date_ && extent.width == desc_.extent.width &&
        extent.height == desc_.extent.height) {
        return;
    }

    pending_extent_ = extent;
    out_of_date_ = true;
    std::fill(acquired_.begin(), acquired_.end(), std::uint8_t{0});
    ++stats_.resizes;
}

void NullSwapchain::set_acquire_timeout(bool enabled) {
    acquire_timeout_ = enabled;
}

const NullSwapchainStats& NullSwapchain::stats() const {
    return stats_;
}

} // namespace tessera::rendering::null
