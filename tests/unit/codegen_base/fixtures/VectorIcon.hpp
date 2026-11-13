// SPDX-License-Identifier: MIT
// Developer class of the `VectorIcon` fixture (ADR-040): the vector codegen tests build it, and the
// CPU paint golden paints it. The generated base and the synthesized asset bundle are produced at
// build time by codegen_base_emit_fixtures.
#pragma once

#include "VectorIcon.g.hpp"
#include "vicon_vector_assets.hpp"

namespace vicon {

class VectorIcon final : public gen::VectorIconBase<VectorIcon> {
private:
    friend struct gen::VectorIconBase<VectorIcon>;
};

} // namespace vicon
