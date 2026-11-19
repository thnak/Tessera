// SPDX-License-Identifier: MIT
//
// The texture-array descriptor set (RHI 1.7, issue #480 part 2). One COMBINED_IMAGE_SAMPLER binding
// holds kTextureArrayCapacity sampled images, and the batched 2D shader picks one with a per-vertex
// index. That is what lets a frame keep one draw when consecutive quads sample different textures:
// the one-set-per-texture path has to end the batch at every switch.
//
// The set is allocated once with the device and rewritten by bind_texture_array, so a frame records
// no descriptor allocation. The layout is created only when create_device() enabled descriptor
// indexing; without it `texture_array_capacity()` is 0 and the renderer keeps the old path.

#include "vulkan_internal.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace tessera::rendering::vulkan {

void VulkanDevice::create_texture_array_layout() {
    if (texture_array_capacity_ == 0) {
        return;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = texture_array_capacity_;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    // PARTIALLY_BOUND lets the shader's array be longer than the number of images a batch binds: a
    // batch with three textures binds three descriptors and the rest stay unwritten, which is legal
    // only because the shader never indexes past the count it was given.
    VkDescriptorBindingFlags binding_flags =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
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
    check_vk(vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &texture_array_layout_),
             "vkCreateDescriptorSetLayout(texture array)");

    // A dedicated pool: the array set is long-lived and must not compete with the per-texture sets
    // the pool list hands out (issue #480 part 1).
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = texture_array_capacity_;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &texture_array_pool_),
             "vkCreateDescriptorPool(texture array)");

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = texture_array_pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &texture_array_layout_;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &texture_array_set_),
             "vkAllocateDescriptorSets(texture array)");
}

void VulkanDevice::destroy_texture_array() {
    if (device_ != VK_NULL_HANDLE) {
        if (texture_array_pool_ != VK_NULL_HANDLE) {
            // The pool owns the set, so destroying it frees both.
            vkDestroyDescriptorPool(device_, texture_array_pool_, nullptr);
        }
        if (texture_array_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, texture_array_layout_, nullptr);
        }
    }
    texture_array_pool_ = VK_NULL_HANDLE;
    texture_array_set_ = VK_NULL_HANDLE;
    texture_array_layout_ = VK_NULL_HANDLE;
    texture_array_capacity_ = 0;
}

void VulkanCommandList::bind_texture_array(std::uint32_t slot,
                                           const std::vector<const rhi::Texture*>& textures) {
    require_usable("bind_texture_array");
    // An empty array is a no-op on every backend, so a caller that resolves no textures this batch
    // does not have to special-case the call (RHI 1.7, issue #480 part 2).
    if (textures.empty()) {
        return;
    }
    if (current_layout_ == VK_NULL_HANDLE) {
        throw std::runtime_error("bind_texture_array: missing pipeline");
    }
    const std::uint32_t capacity = owner_.texture_array_capacity();
    if (capacity == 0) {
        throw std::runtime_error("bind_texture_array: this device has no texture array");
    }
    if (textures.size() > capacity) {
        throw std::invalid_argument("bind_texture_array: " + std::to_string(textures.size()) +
                                    " textures exceed the array capacity " +
                                    std::to_string(capacity));
    }

    // One write per bound image. The set is rewritten in place (UPDATE_AFTER_BIND), so a frame that
    // binds several batches records no allocation; the caller re-binds before each draw that needs
    // a different set, which the renderer does at a batch boundary.
    std::vector<VkDescriptorImageInfo> images(textures.size());
    for (std::size_t index = 0; index < textures.size(); ++index) {
        const VulkanTexture& texture = vulkan_cast<const VulkanTexture>(*textures[index]);
        images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        images[index].imageView = texture.view();
        images[index].sampler = texture.sampler();
    }

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = owner_.texture_array_set();
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = static_cast<std::uint32_t>(images.size());
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = images.data();
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const VkDescriptorSet set = owner_.texture_array_set();
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_GRAPHICS, current_layout_, slot, 1,
                            &set, 0, nullptr);
}

} // namespace tessera::rendering::vulkan
