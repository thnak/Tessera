// SPDX-License-Identifier: MIT
//
// The compute pipeline wrapper (RHI 1.16, issue #683). Kept out of vulkan_internal.hpp and
// vulkan_device_internal.hpp, which are at their size baselines.

#pragma once

#include "vulkan_internal.hpp"

namespace tessera::rendering::vulkan {

class VulkanComputePipeline final : public rhi::ComputePipeline {
public:
    VulkanComputePipeline(VulkanDevice* owner, std::uint32_t epoch, VkDevice device,
                          VkPipeline pipeline, VkPipelineLayout layout,
                          VkDescriptorSetLayout set_layout, VkDescriptorPool pool,
                          VkDescriptorSet set, std::uint32_t storage_buffer_count,
                          std::uint32_t push_constant_size)
        : owner_(owner),
          epoch_(epoch),
          device_(device),
          pipeline_(pipeline),
          layout_(layout),
          set_layout_(set_layout),
          pool_(pool),
          set_(set),
          storage_buffer_count_(storage_buffer_count),
          push_constant_size_(push_constant_size) {}

    ~VulkanComputePipeline() override;

    [[nodiscard]] std::uint32_t storage_buffer_count() const override {
        return storage_buffer_count_;
    }
    [[nodiscard]] VkPipeline handle() const {
        return pipeline_;
    }
    [[nodiscard]] VkPipelineLayout layout() const {
        return layout_;
    }
    [[nodiscard]] VkDescriptorSet set() const {
        return set_;
    }
    [[nodiscard]] std::uint32_t epoch() const {
        return epoch_;
    }
    [[nodiscard]] std::uint32_t push_constant_size() const {
        return push_constant_size_;
    }

private:
    VulkanDevice* owner_ = nullptr;
    std::uint32_t epoch_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    std::uint32_t storage_buffer_count_ = 0;
    std::uint32_t push_constant_size_ = 0;
};

} // namespace tessera::rendering::vulkan
