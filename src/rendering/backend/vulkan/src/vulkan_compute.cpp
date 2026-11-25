// SPDX-License-Identifier: MIT
//
// Compute pipelines and dispatch (RHI 1.16, issue #683, ADR-071). One pipeline owns one descriptor
// set of read/write storage buffers (UPDATE_AFTER_BIND, like the clip-record set), so binding a
// buffer is one descriptor write and a frame records no descriptor allocation.

#include "vulkan_compute.hpp"
#include "vulkan_scope_guard.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace tessera::rendering::vulkan {
namespace {

// vkCmdDispatch's per-dimension limit is at least 65535 on every conformant device.
constexpr std::uint32_t kMaxDispatchCount = 65535;

} // namespace

VulkanComputePipeline::~VulkanComputePipeline() {
    const auto release = [this] {
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_, pipeline_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_, layout_, nullptr);
        }
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_, pool_, nullptr); // frees the set with it
        }
        if (set_layout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        }
    };
    if (owner_ != nullptr) {
        if (owner_->destroyable_epoch(epoch_)) {
            release();
        }
        owner_->resource_destroyed();
        return;
    }
    release();
}

std::unique_ptr<rhi::ComputePipeline>
VulkanDevice::create_compute_pipeline(const rhi::ComputePipelineDesc& desc) {
    require_ready("create_compute_pipeline");
    if (storage_buffer_capacity_ == 0) {
        throw std::runtime_error("create_compute_pipeline: this device has no storage buffers "
                                 "(supports_compute() is false)");
    }
    if (desc.spirv == nullptr || desc.spirv_size == 0 || desc.spirv_size % 4 != 0) {
        throw std::invalid_argument("create_compute_pipeline: SPIR-V is empty or not word-sized");
    }
    if (desc.storage_buffer_count == 0 ||
        desc.storage_buffer_count > rhi::kMaxComputeStorageBuffers ||
        desc.storage_buffer_count > storage_buffer_capacity_) {
        throw std::invalid_argument("create_compute_pipeline: storage_buffer_count " +
                                    std::to_string(desc.storage_buffer_count) + " is out of range");
    }
    if (desc.push_constant_size % 4 != 0 || desc.push_constant_size > 128) {
        throw std::invalid_argument(
            "create_compute_pipeline: push_constant_size must be a multiple of 4 and at most 128");
    }

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = desc.spirv_size;
    shader_info.pCode = desc.spirv;
    VkShaderModule module = VK_NULL_HANDLE;
    check_vk(vkCreateShaderModule(device_, &shader_info, nullptr, &module),
             "vkCreateShaderModule(compute)");
    ScopeGuard module_guard([&] { vkDestroyShaderModule(device_, module, nullptr); });

    VkDescriptorSetLayoutBinding bindings[rhi::kMaxComputeStorageBuffers]{};
    VkDescriptorBindingFlags binding_flags[rhi::kMaxComputeStorageBuffers]{};
    for (std::uint32_t i = 0; i < desc.storage_buffer_count; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binding_flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = desc.storage_buffer_count;
    flags_info.pBindingFlags = binding_flags;
    VkDescriptorSetLayoutCreateInfo set_layout_info{};
    set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout_info.pNext = &flags_info;
    set_layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    set_layout_info.bindingCount = desc.storage_buffer_count;
    set_layout_info.pBindings = bindings;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    check_vk(vkCreateDescriptorSetLayout(device_, &set_layout_info, nullptr, &set_layout),
             "vkCreateDescriptorSetLayout(compute)");
    ScopeGuard set_layout_guard(
        [&] { vkDestroyDescriptorSetLayout(device_, set_layout, nullptr); });

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_size.descriptorCount = desc.storage_buffer_count;
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    check_vk(vkCreateDescriptorPool(device_, &pool_info, nullptr, &pool),
             "vkCreateDescriptorPool(compute)");
    ScopeGuard pool_guard([&] { vkDestroyDescriptorPool(device_, pool, nullptr); });

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    check_vk(vkAllocateDescriptorSets(device_, &allocate, &set),
             "vkAllocateDescriptorSets(compute)");

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = desc.push_constant_size;
    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = desc.push_constant_size > 0 ? 1U : 0U;
    layout_info.pPushConstantRanges = desc.push_constant_size > 0 ? &push_range : nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    check_vk(vkCreatePipelineLayout(device_, &layout_info, nullptr, &layout),
             "vkCreatePipelineLayout(compute)");
    ScopeGuard layout_guard([&] { vkDestroyPipelineLayout(device_, layout, nullptr); });

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check_vk(
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline),
        "vkCreateComputePipelines");
    ScopeGuard pipeline_guard([&] { vkDestroyPipeline(device_, pipeline, nullptr); });

    resource_created();
    auto result = std::make_unique<VulkanComputePipeline>(
        this, epoch_, device_, pipeline, layout, set_layout, pool, set, desc.storage_buffer_count,
        desc.push_constant_size);
    pipeline_guard.dismiss();
    layout_guard.dismiss();
    pool_guard.dismiss();
    set_layout_guard.dismiss();
    return result;
}

void VulkanCommandList::ensure_recording(const char* what) {
    require_usable(what);
    if (render_pass_active_) {
        throw std::logic_error(std::string(what) + ": compute is recorded outside a render pass");
    }
    if (!recording_) {
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(buffer_, &begin_info), "vkBeginCommandBuffer(compute)");
        recording_ = true;
        record_timestamp_begin();
    }
}

void VulkanCommandList::bind_compute_pipeline(const rhi::ComputePipeline& pipeline) {
    ensure_recording("bind_compute_pipeline");
    const auto& vulkan_pipeline = vulkan_cast<const VulkanComputePipeline>(pipeline);
    if (vulkan_pipeline.epoch() != epoch_) {
        throw std::runtime_error(
            "bind_compute_pipeline: pipeline belongs to a lost device generation");
    }
    vkCmdBindPipeline(buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_pipeline.handle());
    const VkDescriptorSet set = vulkan_pipeline.set();
    vkCmdBindDescriptorSets(buffer_, VK_PIPELINE_BIND_POINT_COMPUTE, vulkan_pipeline.layout(), 0, 1,
                            &set, 0, nullptr);
    compute_pipeline_ = &vulkan_pipeline;
}

void VulkanCommandList::bind_compute_buffer(std::uint32_t slot, const rhi::Buffer& storage) {
    ensure_recording("bind_compute_buffer");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("bind_compute_buffer: missing compute pipeline");
    }
    if (slot >= compute_pipeline_->storage_buffer_count()) {
        throw std::invalid_argument(
            "bind_compute_buffer: slot " + std::to_string(slot) + " is outside the pipeline's " +
            std::to_string(compute_pipeline_->storage_buffer_count()) + " storage buffers");
    }
    if (storage.size() == 0) {
        throw std::invalid_argument("bind_compute_buffer: an empty buffer cannot be bound");
    }
    const auto& vulkan_buffer = vulkan_cast<const VulkanBuffer>(storage);
    if (vulkan_buffer.usage() != rhi::BufferUsage::Storage) {
        throw std::invalid_argument(
            "bind_compute_buffer: the buffer was not created with BufferUsage::Storage");
    }
    VkDescriptorBufferInfo info{};
    info.buffer = vulkan_buffer.handle();
    info.offset = 0;
    info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = compute_pipeline_->set();
    write.dstBinding = slot;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
}

void VulkanCommandList::compute_push_constants(const void* data, std::size_t size) {
    ensure_recording("compute_push_constants");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("compute_push_constants: missing compute pipeline");
    }
    if (size == 0 || size > compute_pipeline_->push_constant_size() || size % 4 != 0) {
        throw std::invalid_argument("compute_push_constants: size " + std::to_string(size) +
                                    " does not fit the pipeline's push-constant block");
    }
    vkCmdPushConstants(buffer_, compute_pipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       static_cast<std::uint32_t>(size), data);
}

void VulkanCommandList::dispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    ensure_recording("dispatch");
    if (compute_pipeline_ == nullptr) {
        throw std::logic_error("dispatch: missing compute pipeline");
    }
    if (x == 0 || y == 0 || z == 0 || x > kMaxDispatchCount || y > kMaxDispatchCount ||
        z > kMaxDispatchCount) {
        throw std::invalid_argument("dispatch: workgroup counts must be 1.." +
                                    std::to_string(kMaxDispatchCount));
    }
    vkCmdDispatch(buffer_, x, y, z);
}

void VulkanCommandList::compute_barrier() {
    ensure_recording("compute_barrier");
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                         nullptr);
}

void VulkanCommandList::copy_buffer(const rhi::Buffer& source, rhi::Buffer& destination,
                                    std::size_t size) {
    ensure_recording("copy_buffer");
    const auto& from = vulkan_cast<const VulkanBuffer>(source);
    auto& to = vulkan_cast<VulkanBuffer>(destination);
    if (size == 0 || size > source.size() || size > destination.size()) {
        throw std::invalid_argument("copy_buffer: size " + std::to_string(size) +
                                    " does not fit both buffers");
    }
    // The compute writes must be visible to the copy, and the copy's writes to the host read that
    // follows the submission's completion.
    VkMemoryBarrier before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    before.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(buffer_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    VkBufferCopy region{};
    region.size = size;
    vkCmdCopyBuffer(buffer_, from.handle(), to.handle(), 1, &region);
    VkMemoryBarrier after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                         &after, 0, nullptr, 0, nullptr);
}

} // namespace tessera::rendering::vulkan
