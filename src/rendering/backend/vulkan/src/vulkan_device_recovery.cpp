// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan {

void VulkanDevice::require_ready(const char* what) const {
    if (status_ == rhi::DeviceStatus::Lost) {
        throw std::runtime_error(std::string(what) +
                                 ": device lost; call recover() and recreate resources");
    }
}

void VulkanDevice::mark_device_lost() {
    if (status_ == rhi::DeviceStatus::Lost) {
        return;
    }
    // Bumps the generation so every RHI object created before the loss becomes stale: operations
    // refuse to touch dead handles and destructors stop releasing them.
    status_ = rhi::DeviceStatus::Lost;
    ++epoch_;
    buffer_bytes_ = 0;
    image_bytes_ = 0;
}

#if defined(TESSERA_VULKAN_TEST_HOOKS)
void VulkanDevice::simulate_device_loss_for_testing() {
    mark_device_lost();
    simulated_loss_ = true;
}
#endif

bool VulkanDevice::consume_injected_fault(InjectedDeviceFault fault) {
#if defined(TESSERA_VULKAN_TEST_HOOKS)
    if (injected_fault_ == fault) {
        injected_fault_ = InjectedDeviceFault::NoFault;
        // The injected loss only fakes the driver error; the real VkDevice is still alive, so
        // recovery teardown may destroy its children normally (validation stays clean).
        simulated_loss_ = true;
        return true;
    }
#else
    (void)fault;
#endif
    return false;
}

void VulkanDevice::destroy_device_objects() {
    if (timestamp_query_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, timestamp_query_pool_, nullptr);
    }
    timestamp_query_pool_ = VK_NULL_HANDLE;
    timestamp_capability_ = rhi::GpuTimestampCapability{};
    if (descriptor_pool_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    descriptor_pool_ = VK_NULL_HANDLE;
    if (descriptor_set_layout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
    }
    descriptor_set_layout_ = VK_NULL_HANDLE;
    if (allocator_ != nullptr) {
        vmaDestroyAllocator(allocator_);
        allocator_ = nullptr;
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
    }
    physical_device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
}

std::expected<void, rhi::DeviceRecoveryError> VulkanDevice::recover() {
    if (status_ == rhi::DeviceStatus::Ready) {
        return std::unexpected(rhi::DeviceRecoveryError::NotLost);
    }
    if (recovery_attempted_) {
        // D7 is a one-shot policy: a failed recovery leaves the device Lost until a new loss.
        return std::unexpected(rhi::DeviceRecoveryError::AlreadyAttempted);
    }

    // Release backend-owned state first (it does not belong to the caller and may hold staging
    // buffers/callbacks). A simulated loss keeps the old device alive, so its child objects are
    // destroyed normally (validation stays clean); a real loss abandoned them with the device, so
    // bookkeeping is dropped without Vulkan calls.
    if (simulated_loss_ && device_ != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(device_);
        drain_uploads();
        destroy_command_resources();
    } else {
        pending_uploads_.clear();
        retired_.clear();
        free_commands_.clear();
        inflight_commands_.clear();
    }

    if (live_resources_ != 0) {
        // The caller still holds RHI objects from the lost generation. This does not consume the
        // one-shot attempt: destroy them and call recover() again.
        return std::unexpected(rhi::DeviceRecoveryError::ResourcesOutstanding);
    }
    recovery_attempted_ = true;
#if defined(TESSERA_VULKAN_TEST_HOOKS)
    if (fail_recovery_for_testing_) {
        fail_recovery_for_testing_ = false;
        return std::unexpected(rhi::DeviceRecoveryError::DeviceCreationFailed);
    }
#endif

    if (simulated_loss_) {
        destroy_device_objects();
    } else {
        // A real loss leaves VMA allocations registered; destroying the allocator would touch a
        // dead device, so it is abandoned with it (bounded to one allocator per loss).
        allocator_ = nullptr;
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        physical_device_ = VK_NULL_HANDLE;
        queue_ = VK_NULL_HANDLE;
    }

#if defined(TESSERA_VULKAN_HAS_PRESENT_SURFACE)
    swapchain_extension_enabled_ = false;
    // The resolved family survives the device generation: it was resolved from DeviceDesc/Env, not
    // from the lost device, so recovery probes the same family's queues again.
    win32_present_supported_ = false;
    xlib_present_supported_ = false;
    wayland_present_supported_ = false;
#endif

    try {
        // The instance survives a device loss; the physical device is re-queried because a driver
        // reset can invalidate it. All creation steps are bounded (no waits).
        pick_device();
        create_device();
        create_allocator();
        create_descriptor_layout();
        create_descriptor_pool();
    } catch (...) {
        destroy_device_objects();
        status_ = rhi::DeviceStatus::Lost;
        return std::unexpected(rhi::DeviceRecoveryError::DeviceCreationFailed);
    }

    simulated_loss_ = false;
    recovery_attempted_ = false;
    status_ = rhi::DeviceStatus::Ready;
    return {};
}

} // namespace tessera::rendering::vulkan
