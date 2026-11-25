// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui {

// Bumped whenever any node gains or loses a child. A cache keyed on the shape of the scene (the
// selector tree) compares it to rebuild instead of patching. Process-wide on purpose: it is a
// conservative "something changed shape" signal, never a per-scene identity.
[[nodiscard]] std::uint64_t scene_structure_epoch() noexcept;

// Bumped whenever any node's programmatic `visible()` flips. A walk that skips invisible subtrees
// (interaction sync, the transition bridge) compares it to know whether a node it did not revisit
// could have entered or left the rendered tree since its last whole walk.
[[nodiscard]] std::uint64_t scene_visibility_epoch() noexcept;

// Called by the one setter that flips `visible()`.
void note_scene_visibility_change() noexcept;

} // namespace tessera::ui
