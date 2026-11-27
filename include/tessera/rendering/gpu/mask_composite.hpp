// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::rendering::gpu {

// How one mask layer combines with the layers below it (CSS Masking 1 `mask-composite`, issue
// #617), in the terms of Porter-Duff with the layer as the source and what is below as the
// destination: `Add` is source-over (the layer is just drawn on top), `Subtract` is source-out,
// `Intersect` is source-in and `Exclude` is xor. The result of every operator but `Add` drops the
// destination where the source does not cover it, so it cannot be drawn quad by quad: a layer that
// is not `Add` is drawn into its own target and combined once (offscreen-layers.md section 5.2e).
enum class MaskCompositeOp : std::uint8_t { Add, Subtract, Intersect, Exclude };

} // namespace tessera::rendering::gpu
