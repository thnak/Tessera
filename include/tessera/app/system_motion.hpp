// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::app {

// True when the operating system asks apps to reduce motion (Windows: animation effects turned off
// in Settings, `SPI_GETCLIENTAREAANIMATION`). A driver reads it once at startup and passes it as
// `HostConfig::reduced_motion`, or calls `AppHost::set_reduced_motion` when the setting changes.
// Platforms without a query return false, which keeps animations on.
[[nodiscard]] bool system_prefers_reduced_motion() noexcept;

// Remembers the last reduced-motion value applied to the host so a settings broadcast (which fires
// for unrelated changes too) only reaches `AppHost::set_reduced_motion` when the value moved.
class MotionPreference {
public:
    explicit constexpr MotionPreference(bool initial = false) noexcept
        : reduced_(initial) {}

    // Records `observed`; true exactly when it differs from the previously recorded value.
    constexpr bool update(bool observed) noexcept {
        if (observed == reduced_) {
            return false;
        }
        reduced_ = observed;
        return true;
    }

    [[nodiscard]] constexpr bool reduced() const noexcept {
        return reduced_;
    }

private:
    bool reduced_;
};

} // namespace tessera::app
