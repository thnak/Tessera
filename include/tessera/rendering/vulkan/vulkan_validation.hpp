// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::vulkan {

// Validation messages (warnings + errors) the Khronos validation layer reported to this process'
// debug callback since the process started. The counter is only touched from that callback, so it
// stays at zero - and costs nothing - in a run that does not enable validation. A GPU test reads it
// to assert a frame run was validation-clean instead of eyeballing stderr.
[[nodiscard]] std::uint64_t validation_message_count() noexcept;
void reset_validation_message_count() noexcept;

// True once a device instance was created with the validation layer and its debug messenger
// active. Without it the counter cannot prove anything (no callback is installed), so a test that
// asserts zero messages must check this first and skip with a reason when validation is missing.
[[nodiscard]] bool validation_active() noexcept;

} // namespace tessera::rendering::vulkan
