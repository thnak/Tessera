// SPDX-License-Identifier: MIT
//
// Runs the conformance checks against an opened backend and applies the capability gate that turns
// a missing feature into a skip naming that capability (issue #161).

#pragma once

#include "conformance_backend.hpp"
#include "conformance_summary.hpp"

#include <string>

namespace tessera::tests::gpu::conformance {

// `profile` comes from the opened backend; `backend` is the summary's backend name.
[[nodiscard]] Summary run_suite(rendering::rhi::Device& device, const BackendProfile& profile,
                                std::string backend);

// Every check skipped with "missing capability: device" when no device could be opened.
[[nodiscard]] Summary unavailable_suite(std::string backend);

} // namespace tessera::tests::gpu::conformance
