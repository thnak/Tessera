// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

#include <utility>

namespace tessera::rendering::vulkan {

void VulkanDevice::submit_upload(const std::function<void(VkCommandBuffer)>& record,
                                 std::unique_ptr<rhi::Buffer> staging) {
    reap_uploads();

    const CommandResources commands = acquire_command_resources();
    try {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(commands.buffer, &begin_info),
                 "vkBeginCommandBuffer(upload)");
        record(commands.buffer);
        check_vk(vkEndCommandBuffer(commands.buffer), "vkEndCommandBuffer(upload)");

        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &commands.buffer;
        check_vk(vkQueueSubmit(queue_, 1, &submit_info, commands.fence), "vkQueueSubmit(upload)");
    } catch (...) {
        // Nothing was queued, so the resources are free to reuse right away.
        release_command_resources(commands, false);
        throw;
    }

    pending_uploads_.push_back({commands, std::move(staging), ++upload_serial_});
}

void VulkanDevice::retire_after_uploads(std::function<void()> destroy) {
    reap_uploads();
    if (pending_uploads_.empty()) {
        destroy();
        return;
    }
    // Every pending upload has serial <= upload_serial_, so waiting for that serial covers any
    // upload that could still touch the resource.
    retired_.push_back({std::move(destroy), upload_serial_});
}

void VulkanDevice::reap_uploads() {
    for (auto it = pending_uploads_.begin(); it != pending_uploads_.end();) {
        if (vkGetFenceStatus(device_, it->commands.fence) == VK_NOT_READY) {
            ++it;
            continue;
        }
        // Finished (or the device failed and the copy will never run): the staging buffer and
        // command resources are no longer in use. Erasing destroys the staging buffer.
        release_command_resources(it->commands, false);
        it = pending_uploads_.erase(it);
    }

    // Uploads finish in any order, so the safe point is just below the oldest one still pending.
    std::uint64_t completed = upload_serial_;
    for (const PendingUpload& upload : pending_uploads_) {
        // Plain comparison: volk pulls in windows.h on Windows, whose min() macro breaks std::min.
        if (upload.serial - 1 < completed) {
            completed = upload.serial - 1;
        }
    }

    // Destroyers may free textures whose own teardown calls back into the device, so detach
    // the ready ones before running anything.
    std::vector<RetiredResource> ready;
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (it->serial <= completed) {
            ready.push_back(std::move(*it));
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
    for (RetiredResource& resource : ready) {
        resource.destroy();
    }
}

void VulkanDevice::drain_uploads() {
    // The caller has waited for the device to go idle, so everything pending is finished.
    for (PendingUpload& upload : pending_uploads_) {
        release_command_resources(upload.commands, false);
    }
    pending_uploads_.clear();

    std::vector<RetiredResource> retired = std::move(retired_);
    retired_.clear();
    for (RetiredResource& resource : retired) {
        resource.destroy();
    }
}

} // namespace tessera::rendering::vulkan
