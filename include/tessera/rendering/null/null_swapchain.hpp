// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/swapchain.hpp"

#include <cstdint>
#include <vector>

namespace tessera::rendering::null {

// Deterministic swapchain for tests: no real presentation, no blocking, and no GPU resource.
// Successful acquires rotate through image indices unconditionally; per-image availability is
// not modeled, and set_acquire_timeout() exists so tests can exercise the Timeout path.
struct NullSwapchainStats {
    std::uint32_t acquires = 0;
    std::uint32_t presents = 0;
    std::uint32_t out_of_date = 0;
    std::uint32_t timeouts = 0;
    std::uint32_t invalid_presents = 0;
    std::uint32_t resizes = 0;
};

class NullSwapchain final : public rhi::Swapchain {
public:
    // Throws std::invalid_argument when desc.image_count < 2.
    explicit NullSwapchain(const rhi::SwapchainDesc& desc);

    [[nodiscard]] rhi::Extent2D extent() const override;
    [[nodiscard]] rhi::TextureFormat format() const override;
    [[nodiscard]] std::uint32_t image_count() const override;

    [[nodiscard]] std::expected<std::uint32_t, rhi::SwapchainError>
    acquire_next_image(std::uint64_t timeout_ms) override;
    [[nodiscard]] std::expected<void, rhi::SwapchainError> present(std::uint32_t image) override;
    void resize(rhi::Extent2D extent) override;

    [[nodiscard]] rhi::RenderTarget* image_target(std::uint32_t index) override;

    // Deterministically models a swapchain with no image ready. While enabled,
    // acquire_next_image returns Timeout regardless of its timeout argument.
    void set_acquire_timeout(bool enabled);

    [[nodiscard]] const NullSwapchainStats& stats() const;

private:
    rhi::SwapchainDesc desc_;
    rhi::Extent2D pending_extent_;
    // One flag per image: set by acquire, cleared by present or resize.
    std::vector<std::uint8_t> acquired_;
    std::uint32_t next_image_ = 0;
    bool out_of_date_ = false;
    bool acquire_timeout_ = false;
    NullSwapchainStats stats_;
};

} // namespace tessera::rendering::null
