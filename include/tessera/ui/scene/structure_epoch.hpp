// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui {

// Bumped whenever any node gains or loses a child. A cache keyed on the shape of the scene (the
// selector tree) compares it to rebuild instead of patching. Process-wide on purpose: it is a
// conservative "something changed shape" signal, never a per-scene identity.
[[nodiscard]] std::uint64_t scene_structure_epoch() noexcept;

} // namespace tessera::ui
