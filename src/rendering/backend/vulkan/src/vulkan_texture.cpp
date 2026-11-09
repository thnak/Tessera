// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"

namespace tessera::rendering::vulkan {

std::unique_ptr<rhi::Texture> VulkanDevice::create_texture(const rhi::TextureDesc& desc) {
    const VkFormat format = to_vk_format(desc.format);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {desc.extent.width, desc.extent.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    check_vk(
        vmaCreateImage(allocator_, &image_info, &allocation_info, &image, &allocation, nullptr),
        "vmaCreateImage(texture)");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    check_vk(vkCreateImageView(device_, &view_info, nullptr, &view), "vkCreateImageView");

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk_filter(desc.filter);
    sampler_info.minFilter = sampler_info.magFilter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = to_vk_address(desc.address);
    sampler_info.addressModeV = sampler_info.addressModeU;
    sampler_info.addressModeW = sampler_info.addressModeU;
    sampler_info.maxLod = 0;
    VkSampler sampler = VK_NULL_HANDLE;
    check_vk(vkCreateSampler(device_, &sampler_info, nullptr, &sampler), "vkCreateSampler");

    VkDescriptorSetAllocateInfo descriptor_allocate{};
    descriptor_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptor_allocate.descriptorPool = descriptor_pool_;
    descriptor_allocate.descriptorSetCount = 1;
    descriptor_allocate.pSetLayouts = &descriptor_set_layout_;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &descriptor_allocate, &descriptor_set),
             "vkAllocateDescriptorSets");

    VkDescriptorImageInfo image_descriptor{};
    image_descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_descriptor.imageView = view;
    image_descriptor.sampler = sampler;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_descriptor;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    const std::size_t bpp = bytes_per_pixel(desc.format);
    const std::size_t row_pitch =
        desc.row_pitch != 0 ? desc.row_pitch : static_cast<std::size_t>(desc.extent.width) * bpp;
    const std::size_t staging_size = row_pitch * desc.extent.height;

    if (desc.pixels != nullptr && staging_size > 0) {
        auto staging = create_buffer({staging_size, rhi::BufferUsage::Staging});
        staging->write(desc.pixels, staging_size);
        auto* staging_buffer = dynamic_cast<VulkanBuffer*>(staging.get());
        if (staging_buffer == nullptr) {
            throw std::runtime_error("create_texture: staging buffer allocation failed");
        }

        submit_one_shot([&](VkCommandBuffer command_buffer) {
            record_image_transition(command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = static_cast<std::uint32_t>(row_pitch / (bpp == 0 ? 1 : bpp));
            copy.bufferImageHeight = 0;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.mipLevel = 0;
            copy.imageSubresource.baseArrayLayer = 0;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {desc.extent.width, desc.extent.height, 1};
            vkCmdCopyBufferToImage(command_buffer, staging_buffer->handle(), image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            record_image_transition(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
    } else {
        submit_one_shot([&](VkCommandBuffer command_buffer) {
            record_image_transition(command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        });
    }

    return std::make_unique<VulkanTexture>(device_, allocator_, image, view, sampler, allocation,
                                           descriptor_pool_, descriptor_set, desc.extent,
                                           desc.format, this);
}

void VulkanTexture::write(const void* data, std::size_t size) {
    if (owner_ == nullptr) {
        throw std::runtime_error("VulkanTexture::write: missing device owner");
    }
    owner_->upload_texture(*this, data, size);
}

void VulkanDevice::upload_texture(VulkanTexture& texture, const void* data, std::size_t size) {
    const rhi::Extent2D extent = texture.extent();
    const std::size_t expected =
        static_cast<std::size_t>(extent.width) * extent.height * bytes_per_pixel(texture.format());
    if (data == nullptr || size != expected) {
        throw std::invalid_argument("texture upload must cover the whole tightly packed image");
    }
    if (size == 0) {
        return;
    }

    auto staging = create_buffer({size, rhi::BufferUsage::Staging});
    staging->write(data, size);
    auto* staging_buffer = dynamic_cast<VulkanBuffer*>(staging.get());
    if (staging_buffer == nullptr) {
        throw std::runtime_error("texture upload: staging buffer allocation failed");
    }

    submit_one_shot([&](VkCommandBuffer command_buffer) {
        record_image_transition(command_buffer, texture.image(),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkBufferImageCopy copy{};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.mipLevel = 0;
        copy.imageSubresource.baseArrayLayer = 0;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(command_buffer, staging_buffer->handle(), texture.image(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        record_image_transition(
            command_buffer, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
}

} // namespace tessera::rendering::vulkan
