// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <volk.h>

#include <vk_mem_alloc.h>

namespace tessera::rendering::vulkan {

inline void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed (VkResult=" +
                                 std::to_string(static_cast<int>(result)) + ")");
    }
}

inline VkFormat to_vk_format(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case rhi::TextureFormat::R8Unorm:
        return VK_FORMAT_R8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

inline VkFormat to_vk_vertex_format(rhi::VertexFormat format) {
    switch (format) {
    case rhi::VertexFormat::Float:
        return VK_FORMAT_R32_SFLOAT;
    case rhi::VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case rhi::VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case rhi::VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case rhi::VertexFormat::UByte4Norm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

inline std::size_t bytes_per_pixel(rhi::TextureFormat format) {
    switch (format) {
    case rhi::TextureFormat::R8G8B8A8Unorm:
        return 4;
    case rhi::TextureFormat::R8Unorm:
        return 1;
    }
    return 0;
}

inline VkFilter to_vk_filter(rhi::Filter filter) {
    return filter == rhi::Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

inline VkSamplerAddressMode to_vk_address(rhi::AddressMode mode) {
    return mode == rhi::AddressMode::Repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                                            : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

inline VkAttachmentLoadOp to_vk_load_op(rhi::LoadOp op) {
    switch (op) {
    case rhi::LoadOp::Clear:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    case rhi::LoadOp::Load:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case rhi::LoadOp::DontCare:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
}

inline VkAttachmentStoreOp to_vk_store_op(rhi::StoreOp op) {
    return op == rhi::StoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE
                                     : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

inline void record_image_transition(VkCommandBuffer command_buffer, VkImage image,
                                    VkImageLayout old_layout, VkImageLayout new_layout,
                                    VkAccessFlags src_access, VkAccessFlags dst_access,
                                    VkPipelineStageFlags src_stage,
                                    VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(command_buffer, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

class VulkanBuffer final : public rhi::Buffer {
public:
    VulkanBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation,
                 std::size_t size, void* mapped)
        : allocator_(allocator),
          buffer_(buffer),
          allocation_(allocation),
          size_(size),
          mapped_(mapped) {}

    ~VulkanBuffer() override;

    [[nodiscard]] std::size_t size() const override {
        return size_;
    }
    void write(const void* data, std::size_t size, std::size_t offset) override;
    void read(void* data, std::size_t size, std::size_t offset) const override;
    [[nodiscard]] VkBuffer handle() const {
        return buffer_;
    }

private:
    VmaAllocator allocator_ = nullptr;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    std::size_t size_ = 0;
    void* mapped_ = nullptr;
};

class VulkanDevice;

class VulkanTexture final : public rhi::Texture {
public:
    VulkanTexture(VkDevice device, VmaAllocator allocator, VkImage image, VkImageView view,
                  VkSampler sampler, VmaAllocation allocation, VkDescriptorSet descriptor_set,
                  rhi::Extent2D extent, rhi::TextureFormat format, VulkanDevice* owner)
        : device_(device),
          allocator_(allocator),
          image_(image),
          view_(view),
          sampler_(sampler),
          allocation_(allocation),
          descriptor_set_(descriptor_set),
          extent_(extent),
          format_(format),
          owner_(owner) {}

    ~VulkanTexture() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    void write(const void* data, std::size_t size) override;

    [[nodiscard]] VkImage image() const {
        return image_;
    }
    [[nodiscard]] VkDescriptorSet descriptor_set() const {
        return descriptor_set_;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
    VulkanDevice* owner_ = nullptr;
};

class VulkanPipeline final : public rhi::Pipeline {
public:
    VulkanPipeline(VkDevice device, VkPipeline pipeline, VkPipelineLayout layout)
        : device_(device),
          pipeline_(pipeline),
          layout_(layout) {}

    ~VulkanPipeline() override;

    [[nodiscard]] VkPipeline handle() const {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineLayout layout() const {
        return layout_;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
};

class VulkanRenderTarget final : public rhi::RenderTarget {
public:
    VulkanRenderTarget(VkDevice device, VmaAllocator allocator, VkImage image, VkImageView view,
                       VmaAllocation allocation, rhi::Extent2D extent, rhi::TextureFormat format)
        : device_(device),
          allocator_(allocator),
          image_(image),
          view_(view),
          allocation_(allocation),
          extent_(extent),
          format_(format) {}

    ~VulkanRenderTarget() override;

    [[nodiscard]] rhi::Extent2D extent() const override {
        return extent_;
    }
    [[nodiscard]] rhi::TextureFormat format() const override {
        return format_;
    }
    [[nodiscard]] VkImage image() const {
        return image_;
    }
    [[nodiscard]] VkImageView view() const {
        return view_;
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = nullptr;
    VkImage image_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = nullptr;
    rhi::Extent2D extent_;
    rhi::TextureFormat format_ = rhi::TextureFormat::R8G8B8A8Unorm;
};

class VulkanCommandList final : public rhi::CommandList {
public:
    VulkanCommandList(VkDevice device, VkQueue queue, VkCommandPool pool, VkCommandBuffer buffer)
        : device_(device),
          queue_(queue),
          pool_(pool),
          buffer_(buffer) {}

    ~VulkanCommandList() override;

    void begin_render_pass(const rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc) override;
    void end_render_pass() override;
    void bind_pipeline(const rhi::Pipeline& pipeline) override;
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override;
    void set_viewport(const rhi::Viewport& viewport) override;
    void set_scissor(const rhi::Scissor& scissor) override;
    void push_constants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex = 0) override;
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override;
    void submit() override;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer buffer_ = VK_NULL_HANDLE;
    VkPipelineLayout current_layout_ = VK_NULL_HANDLE;
    const VulkanRenderTarget* current_target_ = nullptr;
    bool recording_ = false;
    bool render_pass_active_ = false;
    bool submitted_ = false;
};

class VulkanDevice final : public rhi::Device {
public:
    explicit VulkanDevice(const rhi::DeviceDesc& desc);
    ~VulkanDevice() override;

    [[nodiscard]] rhi::Backend backend() const override {
        return rhi::Backend::Vulkan;
    }
    std::unique_ptr<rhi::Buffer> create_buffer(const rhi::BufferDesc& desc) override;
    std::unique_ptr<rhi::Texture> create_texture(const rhi::TextureDesc& desc) override;
    std::unique_ptr<rhi::Pipeline> create_pipeline(const rhi::PipelineDesc& desc) override;
    std::unique_ptr<rhi::RenderTarget>
    create_render_target(const rhi::RenderTargetDesc& desc) override;
    std::unique_ptr<rhi::CommandList> create_command_list() override;
    void wait_idle() override;

    // Full-image upload used by Texture::write (dynamic atlases).
    void upload_texture(VulkanTexture& texture, const void* data, std::size_t size);

private:
    void create_instance();
    void pick_device();
    void create_device();
    void create_allocator();
    void create_descriptor_layout();
    void create_descriptor_pool();
    void submit_one_shot(const std::function<void(VkCommandBuffer)>& record);
    void destroy();

    rhi::DeviceDesc desc_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::uint32_t queue_family_ = 0;
    VmaAllocator allocator_ = nullptr;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
};

} // namespace tessera::rendering::vulkan
