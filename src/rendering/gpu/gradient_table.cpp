// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/gradient_table.hpp"

namespace tessera::rendering::gpu {

GradientTable::GradientTable() {
    records_.reserve(gradient::kMaxGradientRecords);
    stop_colors_.reserve(static_cast<std::size_t>(gradient::kMaxGradientStops) * 4);
    stop_offsets_.reserve(gradient::kMaxGradientStops);
}

void GradientTable::reset() {
    records_.clear();
    stop_colors_.clear();
    stop_offsets_.clear();
}

std::optional<std::uint32_t> GradientTable::add(const gradient::Fill& fill) {
    if (records_.size() >= gradient::kMaxGradientRecords ||
        stop_offsets_.size() + fill.stops.size() > gradient::kMaxGradientStops) {
        return std::nullopt;
    }
    gradient::Packed packed;
    if (!gradient::pack(fill, packed)) {
        return std::nullopt;
    }
    packed.record.first_stop = static_cast<std::uint32_t>(stop_offsets_.size());
    const std::size_t count = packed.record.stop_count;
    stop_colors_.insert(stop_colors_.end(), packed.colors.begin(),
                        packed.colors.begin() + count * 4);
    stop_offsets_.insert(stop_offsets_.end(), packed.offsets.begin(),
                         packed.offsets.begin() + count);
    records_.push_back(packed.record);
    return static_cast<std::uint32_t>(records_.size() - 1);
}

} // namespace tessera::rendering::gpu
