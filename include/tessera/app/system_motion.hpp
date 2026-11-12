// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::app {

// True when the operating system asks apps to reduce motion (Windows: animation effects turned off
// in Settings, `SPI_GETCLIENTAREAANIMATION`). A driver reads it once at startup and passes it as
// `HostConfig::reduced_motion`, or calls `AppHost::set_reduced_motion` when the setting changes.
// Platforms without a query return false, which keeps animations on.
[[nodiscard]] bool system_prefers_reduced_motion() noexcept;

} // namespace tessera::app
