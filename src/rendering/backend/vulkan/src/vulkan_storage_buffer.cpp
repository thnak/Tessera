// SPDX-License-Identifier: MIT
//
// The read-only storage-buffer descriptor set (RHI 1.12, ADR-068 slice 1). One STORAGE_BUFFER
// binding in the fragment stage holds the buffer a shader reads, which is the seam `clip-path`
// needs: a clip set is a variable-length record list, and a push constant cannot hold one.
//
// The set is allocated once with the device and rewritten by bind_storage_buffer, so a frame
// records no descriptor allocation. The layout is created only when the device reports a non-zero
// per-stage storage-buffer limit; otherwise `supports_storage_buffers()` is false and the caller
// keeps the push-constant path with its three rounded rectangles (ADR-068 section 5).

#include "vulkan_internal.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan {

void VulkanDevice::create_storage_buffer_layout() {
    if (storage_buffer_capacity_ == 0) {
        return;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // UPDATE_AFTER_BIND lets bind_storage_buffer rewrite the one set while a previously submitted
    // frame may still be reading it. The per-frame-buffer contract (one buffer per frame in flight)
    // is what keeps the *contents* safe; this flag is what keeps the *descriptor write* legal.
    VkDescriptorBindingFlags binding_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = 1;
    flags_info.pBindingFlags = &binding_flags;

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.pNext = &flags_info;
    layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layout_info.bindingCount = 1;
    layout_info.pBindings = &binding;
    check_vk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &storage_buffer_layout_),
             "vkCreateDescriptorSetLayout(storage buffer)");

    // A dedicated pool: the storage set is long-lived and must not compete with the per-texture
    // sets the pool list hands out (issue #480 part 1).
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &storage_buffer_pool_),
             "vkCreateDescriptorPool(storage buffer)");

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = storage_buffer_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &storage_buffer_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &storage_buffer_set_),
             "vkAllocateDescriptorSets(storage buffer)");
}

void VulkanDevice::destroy_storage_buffer() {
    if (device_ != VK_NULL_HANDLE) {
        if (storage_buffer_pool_ != VK_NULL_HANDLE) {
            // The pool owns the set, so destroying it frees both.
            vkDestroyDescriptorPool(device_, storage_buffer_pool_, nullptr);
        }
        if (storage_buffer_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, storage_buffer_layout_, nullptr);
        }
    }
    storage_buffer_pool_ = VK_NULL_HANDLE;
    storage_buffer_set_ = VK_NULL_HANDLE;
    storage_buffer_layout_ = VK_NULL_HANDLE;
    storage_buffer_capacity_ = 0;
}

void VulkanCommandList::bind_storage_buffer(std::uint32_t slot, const rhi::Buffer& storage) {
    require_usable("bind_storage_buffer");
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::logic_error("bind_storage_buffer: missing pipeline");
    }
    if (owner_.storage_buffer_layout() == VK_NULL_HANDLE) {
        throw std::runtime_error("bind_storage_buffer: this device has no storage buffers");
    }
    // The storage set is set 1, so the only legal slot is 1. A caller that asks for another slot is
    // asking for a binding the pipeline layout does not declare.
    if (slot != 1) {
        throw std::invalid_argument("bind_storage_buffer: the storage buffer is set 1, not slot " +
                                    std::to_string(slot));
    }
    if (storage.size() == 0) {
        throw std::invalid_argument("bind_storage_buffer: an empty buffer cannot be read");
    }
    const auto& vulkan_buffer = vulkan_cast<const VulkanBuffer>(storage);
    if (vulkan_buffer.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "bind_storage_buffer: the buffer was not created with BufferUsage::Storage");
    }

    // One write per bind. The set is rewritten in place (UPDATE_AFTER_BIND), so a frame that binds
    // several batches records no allocation; the caller re-binds before each draw that needs a
    // different buffer, which the renderer does at a batch boundary.
    VkDescriptorBufferInfo info{};
    info.buffer = vulkan_buffer.handle();
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = owner_.storage_buffer_set();
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const VkDescriptorSet set = owner_.storage_buffer_set();
    // Set 1, not `slot`: set 0 is the sampled-image layout, and the storage set is a separate set so
    // a shader can sample a texture and read the clip records in the same draw (ADR-068).
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, current_layout_, 1, 1, &set, 0,
                            nullptr);
}

} // namespace tessera::rendering::vulkan
