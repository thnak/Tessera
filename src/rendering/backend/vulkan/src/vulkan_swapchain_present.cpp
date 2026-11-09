// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "vulkan_swapchain.hpp"

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)

#include <algorithm>
#include <cstdint>

namespace tessera::rendering::vulkan {
namespace {

// Acquire/present never waits indefinitely on the UI thread (ADR-021). The caller's timeout is
// clamped to this bound; teardown drains are bounded the same way.
constexpr std::uint64_t kMaxWaitMs = 100;
constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::uint64_t kMaxWaitNs = kMaxWaitMs * kNanosecondsPerMillisecond;

std::uint64_t bounded_timeout_ns(std::uint64_t timeout_ms) {
    if (timeout_ms == 0) {
        return 0;
    }
    const std::uint64_t clamped = std::min(timeout_ms, kMaxWaitMs);
    return clamped * kNanosecondsPerMillisecond;
}

} // namespace

void VulkanSwapchain::wait_for_present_bounded() {
    if (!present_submitted_ && !drain_pending_) {
        return;
    }
    if (drain_fence_ == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(device_, &fence_info, nullptr, &drain_fence_) != VK_SUCCESS) {
            return;
        }
    }
    if (drain_pending_) {
        if (vkWaitForFences(device_, 1, &drain_fence_, VK_TRUE, kMaxWaitNs) != VK_SUCCESS) {
            return;
        }
        drain_pending_ = false;
        (void)vkResetFences(device_, 1, &drain_fence_);
    }

    // An empty submit is an ordering barrier: its fence signals only after earlier queue work,
    // so this drains the last present without vkDeviceWaitIdle on the UI thread.
    if (vkQueueSubmit(queue_, 0, nullptr, drain_fence_) != VK_SUCCESS) {
        return;
    }
    drain_pending_ = true;
    if (vkWaitForFences(device_, 1, &drain_fence_, VK_TRUE, kMaxWaitNs) == VK_SUCCESS) {
        drain_pending_ = false;
        present_submitted_ = false;
        (void)vkResetFences(device_, 1, &drain_fence_);
    }
}

void VulkanSwapchain::destroy() {
    if (owner_ == nullptr || owner_->destroyable_epoch(epoch_)) {
        wait_for_present_bounded();
        destroy_swapchain_resources();
        // Never destroy a fence that may still be owned by a pending queue operation.
        if (acquire_fence_ != VK_NULL_HANDLE && !acquire_pending_) {
            vkDestroyFence(device_, acquire_fence_, nullptr);
        }
        acquire_fence_ = VK_NULL_HANDLE;
        if (drain_fence_ != VK_NULL_HANDLE && !drain_pending_) {
            vkDestroyFence(device_, drain_fence_, nullptr);
        }
        drain_fence_ = VK_NULL_HANDLE;
    } else {
        // Lost device generation: the swapchain, image views, semaphores and fences died with the
        // old VkDevice, so only the host-side bookkeeping is dropped here.
        image_targets_.clear();
        render_finished_.clear();
        views_.clear();
        images_.clear();
        image_acquired_.clear();
        acquire_fence_ = VK_NULL_HANDLE;
        drain_fence_ = VK_NULL_HANDLE;
        acquire_pending_ = false;
        present_submitted_ = false;
        drain_pending_ = false;
    }
    if (surface_ != VK_NULL_HANDLE && instance_ != VK_NULL_HANDLE) {
        // The surface belongs to the instance, which survives a device loss.
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
}

std::expected<std::uint32_t, rhi::SwapchainError>
VulkanSwapchain::acquire_next_image(std::uint64_t timeout_ms) {
    if (!usable() || swapchain_ == VK_NULL_HANDLE) {
        return std::unexpected(usable() ? rhi::SwapchainError::NotSupported
                                        : rhi::SwapchainError::DeviceLost);
    }
    if (owner_ != nullptr &&
        owner_->consume_injected_fault(InjectedDeviceFault::DeviceLostOnAcquire)) {
        owner_->mark_device_lost();
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }

    if (out_of_date_) {
        const auto recreated = recreate_pending();
        if (!recreated) {
            return std::unexpected(recreated.error());
        }
        ++stats_.out_of_date;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }

    if (acquire_pending_) {
        const VkResult status = vkGetFenceStatus(device_, acquire_fence_);
        if (status == VK_NOT_READY) {
            ++stats_.timeouts;
            return std::unexpected(rhi::SwapchainError::Timeout);
        }
        if (status != VK_SUCCESS) {
            acquire_pending_ = false;
            if (status == VK_ERROR_DEVICE_LOST) {
                if (owner_ != nullptr) {
                    owner_->mark_device_lost();
                }
                return std::unexpected(rhi::SwapchainError::DeviceLost);
            }
            return std::unexpected(rhi::SwapchainError::SurfaceLost);
        }
        (void)vkResetFences(device_, 1, &acquire_fence_);
        acquire_pending_ = false;
        if (pending_acquire_index_ >= image_acquired_.size()) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        image_acquired_[pending_acquire_index_] = 1;
        ++stats_.acquires;
        return pending_acquire_index_;
    }

    ensure_acquire_fence();
    if (acquire_fence_ == VK_NULL_HANDLE) {
        return std::unexpected(rhi::SwapchainError::NotSupported);
    }

    std::uint32_t image = 0;
    const VkResult result =
        vkAcquireNextImageKHR(device_, swapchain_, bounded_timeout_ns(timeout_ms), VK_NULL_HANDLE,
                              acquire_fence_, &image);
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
        const VkResult waited = vkWaitForFences(device_, 1, &acquire_fence_, VK_TRUE, kMaxWaitNs);
        if (waited != VK_SUCCESS) {
            acquire_pending_ = true;
            pending_acquire_index_ = image;
            if (waited == VK_ERROR_DEVICE_LOST) {
                if (owner_ != nullptr) {
                    owner_->mark_device_lost();
                }
                return std::unexpected(rhi::SwapchainError::DeviceLost);
            }
            ++stats_.timeouts;
            return std::unexpected(rhi::SwapchainError::Timeout);
        }
        (void)vkResetFences(device_, 1, &acquire_fence_);
        if (image >= image_acquired_.size()) {
            return std::unexpected(rhi::SwapchainError::NotSupported);
        }
        image_acquired_[image] = 1;
        ++stats_.acquires;
        if (result == VK_SUBOPTIMAL_KHR) {
            ++stats_.suboptimal;
        }
        return image;
    }
    if (result == VK_TIMEOUT || result == VK_NOT_READY) {
        ++stats_.timeouts;
        return std::unexpected(rhi::SwapchainError::Timeout);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        const auto recreated = recreate_pending();
        if (!recreated) {
            return std::unexpected(recreated.error());
        }
        ++stats_.out_of_date;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    if (result == VK_ERROR_DEVICE_LOST) {
        if (owner_ != nullptr) {
            owner_->mark_device_lost();
        }
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
    return std::unexpected(rhi::SwapchainError::NotSupported);
}

std::expected<void, rhi::SwapchainError> VulkanSwapchain::present(std::uint32_t image) {
    if (!usable()) {
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
    if (owner_ != nullptr &&
        owner_->consume_injected_fault(InjectedDeviceFault::DeviceLostOnPresent)) {
        owner_->mark_device_lost();
        return std::unexpected(rhi::SwapchainError::DeviceLost);
    }
    if (swapchain_ == VK_NULL_HANDLE || image >= image_acquired_.size() ||
        image_acquired_[image] == 0) {
        ++stats_.invalid_presents;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }

    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &image;

    // Wait for the GPU to finish rendering this image (signaled by the render submission), not
    // the CPU. An image presented without being rendered has nothing to wait for.
    const VulkanRenderTarget& target = *image_targets_[image];
    VkSemaphore wait_semaphore = target.render_finished_semaphore();
    if (target.render_pending() && wait_semaphore != VK_NULL_HANDLE) {
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &wait_semaphore;
    }

    const VkResult result = vkQueuePresentKHR(queue_, &present_info);
    target.set_render_pending(false);
    image_acquired_[image] = 0;
    ++stats_.presents;
    present_submitted_ = true;

    if (result == VK_SUCCESS) {
        return {};
    }
    if (result == VK_SUBOPTIMAL_KHR) {
        ++stats_.suboptimal;
        return std::unexpected(rhi::SwapchainError::Suboptimal);
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        out_of_date_ = true;
        resize_requested_ = false;
        ++stats_.out_of_date;
        return std::unexpected(rhi::SwapchainError::OutOfDate);
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        return std::unexpected(rhi::SwapchainError::SurfaceLost);
    }
    return std::unexpected(rhi::SwapchainError::NotSupported);
}

void VulkanSwapchain::resize(rhi::Extent2D extent) {
    if (extent.width == 0 || extent.height == 0) {
        return;
    }
    if (!out_of_date_ && extent.width == extent_.width && extent.height == extent_.height) {
        return;
    }
    pending_extent_ = extent;
    resize_requested_ = true;
    out_of_date_ = true;
    ++stats_.resizes;
}

rhi::RenderTarget* VulkanSwapchain::image_target(std::uint32_t index) {
    if (index >= image_targets_.size()) {
        return nullptr;
    }
    return image_targets_[index].get();
}

} // namespace tessera::rendering::vulkan

#endif // TESSERA_VULKAN_HAS_PRESENT_SURFACE
