// SPDX-License-Identifier: MIT

#include "vulkan_internal.hpp"
#include "vulkan_scope_guard.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::rendering::vulkan {

std::unique_ptr<rhi::Texture> VulkanDevice::create_texture(const rhi::TextureDesc& desc) {
    require_ready("create_texture");
    const std::uint32_t sample_count = checked_sample_count(desc.sample_count, "create_texture");
    if (sample_count > 1 && desc.pixels != nullptr) {
        throw std::invalid_argument(
            "create_texture: pixels cannot be uploaded to a multisampled texture");
    }
    if (sample_count > 1 && !supports_sample_count(desc.format, sample_count)) {
        throw std::runtime_error("create_texture: " + std::to_string(sample_count) +
                                 "x MSAA is not supported for this texture format");
    }
    if (!rhi::valid_texture_levels(desc)) {
        throw std::invalid_argument("create_texture: invalid mip level count");
    }
    if (desc.levels > 1 && !supports_texture_mipmaps(desc.format)) {
        throw std::runtime_error("create_texture: mip chains are not supported for this format");
    }
    const std::uint32_t levels = desc.levels;
    const VkFormat format = to_vk_format(desc.format);

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {desc.extent.width, desc.extent.height, 1};
    image_info.mipLevels = levels;
    image_info.arrayLayers = 1;
    image_info.samples = to_vk_sample_count(sample_count);
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    if (sample_count > 1) {
        // Multisampled textures are storage/attachment-style resources; the 2D pipeline samples
        // 1-sample textures only (sampler2D vs sampler2DMS).
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    } else {
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (levels > 1) {
            // Each level of the chain is blitted from the one above it.
            image_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }
    }
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_AUTO;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    check_vk(
        vmaCreateImage(allocator_, &image_info, &allocation_info, &image, &allocation, nullptr),
        "vmaCreateImage(texture)");
    ScopeGuard image_guard([&] { vmaDestroyImage(allocator_, image, allocation); });

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = levels;
    view_info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    check_vk(vkCreateImageView(device_, &view_info, nullptr, &view), "vkCreateImageView");
    ScopeGuard view_guard([&] { vkDestroyImageView(device_, view, nullptr); });

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = to_vk_filter(desc.filter);
    sampler_info.minFilter = sampler_info.magFilter;
    // A single-level texture never samples another level; a chain picks a level from the draw's
    // minification, blending neighbours only when the texture itself is filtered linearly.
    sampler_info.mipmapMode = desc.filter == rhi::Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                 : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = to_vk_address(desc.address);
    sampler_info.addressModeV = sampler_info.addressModeU;
    sampler_info.addressModeW = sampler_info.addressModeU;
    sampler_info.maxLod = static_cast<float>(levels - 1U);
    VkSampler sampler = VK_NULL_HANDLE;
    check_vk(vkCreateSampler(device_, &sampler_info, nullptr, &sampler), "vkCreateSampler");
    ScopeGuard sampler_guard([&] { vkDestroySampler(device_, sampler, nullptr); });

    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set = allocate_descriptor_set("create_texture", descriptor_pool);
    ScopeGuard set_guard(
        [&] { vkFreeDescriptorSets(device_, descriptor_pool, 1, &descriptor_set); });

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

    // From here the texture owns every handle: if the upload below throws, its destructor
    // releases them (deferred past any upload already queued), so the guards step aside.
    resource_created();
    account_image(texture_bytes(desc.extent, desc.format, sample_count, levels), epoch_);
    auto texture = std::make_unique<VulkanTexture>(
        device_, allocator_, image, view, sampler, allocation, descriptor_pool, descriptor_set,
        desc.extent, desc.format, this, epoch_, sample_count, levels);
    set_guard.dismiss();
    sampler_guard.dismiss();
    view_guard.dismiss();
    image_guard.dismiss();

    const std::size_t bpp = bytes_per_pixel(desc.format);
    const std::size_t row_pitch =
        desc.row_pitch != 0 ? desc.row_pitch : static_cast<std::size_t>(desc.extent.width) * bpp;
    const std::size_t staging_size = row_pitch * desc.extent.height;

    if (desc.pixels != nullptr && staging_size > 0) {
        auto staging = create_buffer({staging_size, rhi::BufferUsage::Staging});
        staging->write(desc.pixels, staging_size);
        auto* staging_buffer = &vulkan_cast<VulkanBuffer>(*staging);

        // Queued without waiting; the device keeps the staging buffer until the copy finishes.
        submit_upload(
            [&](VkCommandBuffer command_buffer) {
                if (levels > 1) {
                    record_texture_upload_with_mips(
                        command_buffer, image, staging_buffer->handle(), desc.extent,
                        static_cast<std::uint32_t>(row_pitch / (bpp == 0 ? 1 : bpp)), levels);
                    return;
                }
                record_image_transition(
                    command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

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
            },
            std::move(staging));
    } else {
        submit_upload(
            [&](VkCommandBuffer command_buffer) {
                if (sample_count > 1) {
                    // Multisample storage texture (no upload): move it to the attachment layout.
                    record_image_transition(command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    return;
                }
                record_image_transition(
                    command_buffer, image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            },
            nullptr);
    }

    return texture;
}

void VulkanTexture::write(const void* data, std::size_t size) {
    if (owner_ == nullptr) {
        throw std::runtime_error("VulkanTexture::write: missing device owner");
    }
    owner_->upload_texture(*this, data, size);
}

void VulkanTexture::write_region(const rhi::TextureRegion& region, const void* data,
                                 std::size_t row_pitch) {
    if (owner_ == nullptr) {
        throw std::runtime_error("VulkanTexture::write_region: missing device owner");
    }
    owner_->upload_texture_region(*this, region, data, row_pitch);
}

void VulkanDevice::upload_texture(VulkanTexture& texture, const void* data, std::size_t size) {
    const rhi::Extent2D extent = texture.extent();
    const std::size_t expected =
        static_cast<std::size_t>(extent.width) * extent.height * bytes_per_pixel(texture.format());
    if (data == nullptr || size != expected) {
        throw std::invalid_argument("texture upload must cover the whole tightly packed image");
    }
    upload_texture_region(texture, rhi::TextureRegion{0, 0, extent.width, extent.height}, data, 0);
}

void VulkanDevice::upload_texture_region(VulkanTexture& texture, const rhi::TextureRegion& region,
                                         const void* data, std::size_t row_pitch) {
    if (!usable_epoch(texture.epoch())) {
        throw std::runtime_error("upload_texture: texture belongs to a lost device generation");
    }
    if (texture.sample_count() != 1) {
        throw std::invalid_argument("upload_texture: multisampled textures cannot be uploaded");
    }
    if (texture.levels() != 1) {
        throw std::invalid_argument("upload_texture: mipmapped textures are immutable");
    }
    const std::size_t pixel_bytes = bytes_per_pixel(texture.format());
    const std::size_t pitch =
        rhi::checked_region_pitch(texture.extent(), pixel_bytes, region, row_pitch);
    if (region.empty()) {
        return;
    }
    if (data == nullptr) {
        throw std::invalid_argument("upload_texture: region upload without data");
    }

    // Stage only the region, tightly packed: the copy then needs no row length, and a pitched
    // source (an atlas page) is repacked row by row into mapped memory instead of staging the rows
    // around the rectangle.
    const std::size_t row_bytes = static_cast<std::size_t>(region.width) * pixel_bytes;
    const std::size_t size = row_bytes * region.height;
    auto staging = create_buffer({size, rhi::BufferUsage::Staging});
    if (pitch == row_bytes) {
        staging->write(data, size);
    } else {
        for (std::uint32_t row = 0; row < region.height; ++row) {
            staging->write(static_cast<const std::uint8_t*>(data) +
                               static_cast<std::size_t>(row) * pitch,
                           row_bytes, static_cast<std::size_t>(row) * row_bytes);
        }
    }
    auto* staging_buffer = &vulkan_cast<VulkanBuffer>(*staging);

    submit_upload(
        [&](VkCommandBuffer command_buffer) {
            record_image_transition(
                command_buffer, texture.image(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
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
            copy.imageOffset = {static_cast<std::int32_t>(region.x),
                                static_cast<std::int32_t>(region.y), 0};
            copy.imageExtent = {region.width, region.height, 1};
            vkCmdCopyBufferToImage(command_buffer, staging_buffer->handle(), texture.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            record_image_transition(
                command_buffer, texture.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        },
        std::move(staging));
}

} // namespace tessera::rendering::vulkan
