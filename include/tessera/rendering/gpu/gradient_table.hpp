// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/gradient_math.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace tessera::rendering::gpu {

// The frame's gradient records and stop ramps, packed in the layout the fragment shader reads
// (gradient_records.hpp). Append-only within a frame and reset at frame start, exactly like the
// clip record list, so ClipBuffer can upload only what was added since its last upload. The vectors
// are reserved to the frame capacity once, so a frame never allocates here.
class GradientTable {
public:
    GradientTable();

    void reset();

    // Packs `fill` and appends it; the record's index, or nullopt when the fill cannot be drawn or
    // the frame's records or stops are full (the caller paints it banded instead).
    [[nodiscard]] std::optional<std::uint32_t> add(const gradient::Fill& fill);

    [[nodiscard]] const std::vector<gradient::GradientRecord>& records() const {
        return records_;
    }
    // Four floats per stop, then one offset per stop, both indexed by a record's `first_stop`.
    [[nodiscard]] const std::vector<float>& stop_colors() const {
        return stop_colors_;
    }
    [[nodiscard]] const std::vector<float>& stop_offsets() const {
        return stop_offsets_;
    }
    [[nodiscard]] std::uint32_t stop_count() const {
        return static_cast<std::uint32_t>(stop_offsets_.size());
    }

private:
    std::vector<gradient::GradientRecord> records_;
    std::vector<float> stop_colors_;
    std::vector<float> stop_offsets_;
};

} // namespace tessera::rendering::gpu
