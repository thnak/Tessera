// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <memory>

namespace tessera::rendering::vulkan {

std::unique_ptr<rhi::Device> create_vulkan_device(const rhi::DeviceDesc& desc);

} // namespace tessera::rendering::vulkan
