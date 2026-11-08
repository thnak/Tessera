// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_metrics.hpp"

namespace tessera::ui::text {

// Baseline metrics for one loaded face. A style selects a face via its font stack; until the
// stack engine lands, the caller binds the provider to the face it resolved.
class FreeTypeFontMetricsProvider final : public FontMetricsProvider {
public:
    explicit FreeTypeFontMetricsProvider(FontFace face);

    [[nodiscard]] FontMetrics metrics(const TextStyle& style) const override;

private:
    FontFace face_;
};

} // namespace tessera::ui::text
