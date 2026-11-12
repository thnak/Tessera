// SPDX-License-Identifier: MIT
//
// Process-wide validation message counter (include/tessera/rendering/vulkan/vulkan_validation.hpp).
// The debug callback is the only writer; relaxed atomics keep the increment cheap enough to run for
// every message while validation is on, and nothing runs at all when it is off.

#include "tessera/rendering/vulkan/vulkan_validation.hpp"

#include <atomic>

namespace tessera::rendering::vulkan {
namespace {

std::atomic<std::uint64_t> g_messages{0};
std::atomic<bool> g_active{false};

} // namespace

std::uint64_t validation_message_count() noexcept {
    return g_messages.load(std::memory_order_relaxed);
}

void reset_validation_message_count() noexcept {
    g_messages.store(0, std::memory_order_relaxed);
}

bool validation_active() noexcept {
    return g_active.load(std::memory_order_relaxed);
}

// Called by the Vulkan debug callback (vulkan_instance.cpp) for every warning/error.
void record_validation_message() noexcept {
    g_messages.fetch_add(1, std::memory_order_relaxed);
}

// Called once an instance created the validation debug messenger.
void mark_validation_active() noexcept {
    g_active.store(true, std::memory_order_relaxed);
}

} // namespace tessera::rendering::vulkan
