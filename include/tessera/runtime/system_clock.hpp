// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/clock.hpp"

namespace tessera::runtime {

// Clock adapter backed by the HAL steady clock; deterministic tests keep using
// ManualClock.
class SystemClock final : public Clock {
public:
    [[nodiscard]] double now_seconds() const override;
};

} // namespace tessera::runtime
