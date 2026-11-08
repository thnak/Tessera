// SPDX-License-Identifier: MIT

#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/null/null_swapchain.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::rendering::null::NullSwapchain;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Swapchain;
using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;
using tessera::rendering::rhi::TextureFormat;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "swapchain: " << message << "\n";
    }
    return condition;
}

SwapchainDesc make_desc(std::uint32_t image_count = 2) {
    SwapchainDesc desc;
    desc.extent = {640, 480};
    desc.format = TextureFormat::R8G8B8A8Unorm;
    desc.image_count = image_count;
    desc.vsync = true;
    return desc;
}

std::unique_ptr<Swapchain> create(Device& device, const SwapchainDesc& desc) {
    auto created = device.create_swapchain(desc);
    if (!created) {
        return nullptr;
    }
    return std::move(*created);
}

NullSwapchain* as_null(Swapchain& swapchain) {
    return dynamic_cast<NullSwapchain*>(&swapchain);
}

} // namespace

int main() {
    using namespace tessera::rendering;

    auto device = null::create_null_device(rhi::DeviceDesc{});

    // Descriptor defaults are part of the seam contract.
    const SwapchainDesc defaults;
    if (!expect(defaults.image_count == 2, "default image_count") ||
        !expect(defaults.vsync, "default vsync") ||
        !expect(defaults.format == TextureFormat::R8G8B8A8Unorm, "default format")) {
        return 1;
    }

    // Fewer than two images cannot form a swapchain. Both the descriptor path and direct
    // construction reject it.
    for (std::uint32_t image_count : {0U, 1U}) {
        auto result = device->create_swapchain(make_desc(image_count));
        if (!expect(!result.has_value(), "invalid image_count rejected") ||
            !expect(result.error() == SwapchainError::NotSupported,
                    "invalid image_count reports NotSupported")) {
            return 2;
        }
    }

    bool direct_threw = false;
    try {
        const NullSwapchain direct(make_desc(1));
        (void)direct;
    } catch (const std::invalid_argument&) {
        direct_threw = true;
    }
    if (!expect(direct_threw, "NullSwapchain rejects invalid image_count")) {
        return 3;
    }

    {
        auto swapchain = create(*device, make_desc());
        if (!expect(swapchain != nullptr, "valid desc creates a swapchain")) {
            return 4;
        }
        if (!expect(swapchain->extent().width == 640 && swapchain->extent().height == 480,
                    "initial extent") ||
            !expect(swapchain->format() == TextureFormat::R8G8B8A8Unorm, "initial format") ||
            !expect(swapchain->image_count() == 2, "initial image count")) {
            return 5;
        }
        auto* null_swapchain = as_null(*swapchain);
        if (!expect(null_swapchain != nullptr, "null device returns NullSwapchain")) {
            return 6;
        }

        // Acquire rotates through images regardless of presentation, so tests can drive a fixed
        // sequence without any GPU work.
        const auto first = swapchain->acquire_next_image(0);
        const auto second = swapchain->acquire_next_image(0);
        const auto third = swapchain->acquire_next_image(0);
        if (!expect(first.has_value() && *first == 0, "rotation starts at image 0") ||
            !expect(second.has_value() && *second == 1, "rotation returns image 1") ||
            !expect(third.has_value() && *third == 0, "rotation wraps to image 0")) {
            return 7;
        }
        if (!expect(null_swapchain->stats().acquires == 3, "acquire count")) {
            return 8;
        }
    }

    {
        auto swapchain = create(*device, make_desc());
        auto* null_swapchain = as_null(*swapchain);
        if (!expect(swapchain != nullptr && null_swapchain != nullptr, "fresh swapchain")) {
            return 9;
        }

        // Presenting before any acquire is rejected and retires nothing.
        const auto missing = swapchain->present(0);
        if (!expect(!missing.has_value(), "present without acquire rejected") ||
            !expect(missing.error() == SwapchainError::OutOfDate, "missing image is OutOfDate")) {
            return 10;
        }
        if (!expect(null_swapchain->stats().presents == 0, "missing present not counted") ||
            !expect(null_swapchain->stats().invalid_presents == 1,
                    "missing present counted as invalid")) {
            return 11;
        }

        if (!expect(swapchain->acquire_next_image(0).has_value(), "acquire for present")) {
            return 12;
        }
        if (!expect(swapchain->present(0).has_value(), "acquired image presents") ||
            !expect(null_swapchain->stats().presents == 1, "present counted")) {
            return 13;
        }

        // A presented image is retired: presenting it again is a duplicate and out of range
        // indices are invalid as well.
        const auto duplicate = swapchain->present(0);
        const auto out_of_range = swapchain->present(99);
        if (!expect(!duplicate.has_value() && duplicate.error() == SwapchainError::OutOfDate,
                    "duplicate present rejected") ||
            !expect(!out_of_range.has_value() && out_of_range.error() == SwapchainError::OutOfDate,
                    "out-of-range present rejected")) {
            return 14;
        }
        if (!expect(null_swapchain->stats().presents == 1, "rejected presents not counted") ||
            !expect(null_swapchain->stats().invalid_presents == 3,
                    "rejected presents counted as invalid")) {
            return 15;
        }
    }

    {
        auto swapchain = create(*device, make_desc());
        auto* null_swapchain = as_null(*swapchain);
        if (!expect(swapchain != nullptr && null_swapchain != nullptr, "resize swapchain")) {
            return 16;
        }

        if (!expect(swapchain->acquire_next_image(0).has_value(), "acquire before resize")) {
            return 17;
        }
        swapchain->resize({800, 600});
        if (!expect(null_swapchain->stats().resizes == 1, "resize counted")) {
            return 18;
        }

        // The in-flight acquisition belongs to the old size and is retired by the resize.
        if (!expect(!swapchain->present(0).has_value(), "resize retires acquired images")) {
            return 19;
        }

        const auto stale = swapchain->acquire_next_image(1000);
        if (!expect(!stale.has_value() && stale.error() == SwapchainError::OutOfDate,
                    "first acquire after resize is OutOfDate") ||
            !expect(swapchain->extent().width == 800 && swapchain->extent().height == 600,
                    "out-of-date acquire adopts pending extent") ||
            !expect(null_swapchain->stats().out_of_date == 1, "out-of-date counted")) {
            return 20;
        }

        const auto adopted = swapchain->acquire_next_image(1000);
        if (!expect(adopted.has_value() && *adopted == 0, "retry acquires image 0") ||
            !expect(swapchain->extent().width == 800 && swapchain->extent().height == 600,
                    "retry adopts new extent")) {
            return 21;
        }

        // A same-size resize does not force another OutOfDate.
        swapchain->resize({800, 600});
        if (!expect(null_swapchain->stats().resizes == 1, "same-size resize is a no-op")) {
            return 22;
        }
        const auto next = swapchain->acquire_next_image(0);
        if (!expect(next.has_value() && *next == 1, "rotation continues after resize")) {
            return 23;
        }
    }

    {
        auto swapchain = create(*device, make_desc());
        auto* null_swapchain = as_null(*swapchain);
        if (!expect(swapchain != nullptr && null_swapchain != nullptr, "timeout swapchain")) {
            return 24;
        }

        null_swapchain->set_acquire_timeout(true);
        for (std::uint64_t timeout : {std::uint64_t{0}, std::uint64_t{1000}}) {
            const auto timed_out = swapchain->acquire_next_image(timeout);
            if (!expect(!timed_out.has_value() && timed_out.error() == SwapchainError::Timeout,
                        "configured acquire timeout")) {
                return 25;
            }
        }
        if (!expect(null_swapchain->stats().timeouts == 2, "timeouts counted") ||
            !expect(null_swapchain->stats().acquires == 0, "timeout does not acquire")) {
            return 26;
        }

        null_swapchain->set_acquire_timeout(false);
        const auto recovered = swapchain->acquire_next_image(0);
        if (!expect(recovered.has_value() && *recovered == 0,
                    "clearing timeout resumes rotation at image 0")) {
            return 27;
        }
    }

    std::cout << "swapchain: ok\n";
    return 0;
}
