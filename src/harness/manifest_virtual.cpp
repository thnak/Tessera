// SPDX-License-Identifier: MIT
//
// The live virtualization seam behind the manifest scene's `VirtualList`/`VirtualGrid` nodes
// (`docs/specs/headless-automation-v1.md` section 6.8). The manifest describes structure only, so
// each such node gets a fixed synthetic collection: 100 items, 16 px main extent, four grid
// columns. The realized item nodes carry the collection's `uiId` and the zero-based index as their
// item key, which is what makes `scrollIntoView` + a follow-up snapshot/tap provable.

#include "manifest_virtual.hpp"
#include "tessera/ui/virtual/grid_rows.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace tessera::automation {
namespace {

namespace ui = tessera::ui;
namespace virt = tessera::ui::virtualization;

// Fixed synthetic collection shape. The manifest has no item model, and inventing one would need a
// manifest schema change; these constants are the documented structural stand-in.
constexpr std::size_t kItemCount = 100;
constexpr float kItemMainExtent = 16.0f;
constexpr std::size_t kGridColumns = kManifestVirtualGridColumns;
constexpr std::size_t kListPoolCap = 64;
constexpr std::size_t kStateCapacity = 32;

float viewport_extent(const ui::Node& container) {
    return std::max(container.bounds().height, 0.0f);
}

ui::PaintData item_paint(std::size_t index) {
    ui::PaintData data;
    data.kind = ui::PaintKind::Text;
    data.text = "Item " + std::to_string(index);
    return data;
}

// Shared item-node identity: the collection's uiId and the item's key, plus a unique runtime id so
// layout boxes and keyed focus never collapse two realized items onto one id.
void configure_item(ui::Node& node, const ui::Node& container, std::size_t index,
                    std::uint64_t id) {
    node.set_id(id);
    node.set_element_kind("VirtualItem");
    node.set_automation_id(container.automation_id());
    node.set_item_key(index);
    node.set_focusable(true);
    node.set_paint_data(item_paint(index));
}

// One list collection: a fixed `VirtualRange` over a synthetic source. `VirtualRange` owns the
// window math, recycling, and viewport-relative placement.
class ManifestListCollection final : public VirtualCollection {
public:
    ManifestListCollection(ui::Node& container, std::uint64_t& next_node_id)
        : container_(container),
          source_(container, next_node_id),
          range_(range_config(), source_, container) {
        (void)range_.sync();
    }

    [[nodiscard]] ui::Node& container() noexcept override {
        return container_;
    }
    [[nodiscard]] std::size_t item_count() const noexcept override {
        return range_.item_count();
    }
    [[nodiscard]] float scroll_offset() const noexcept override {
        return scroll_offset_;
    }

    [[nodiscard]] std::expected<VirtualScrollOutcome, VirtualCollectionError>
    scroll_to_index(std::size_t index, virt::ScrollAlignment alignment) override {
        if (index >= item_count()) {
            return std::unexpected(VirtualCollectionError::IndexOutOfRange);
        }
        initialized_ = true;
        const auto target = range_.scroll_to_index(index, viewport_extent(container_), alignment);
        if (!target.has_value()) {
            return std::unexpected(VirtualCollectionError::IndexOutOfRange);
        }
        const auto updated = range_.update(target->scroll_offset, viewport_extent(container_));
        if (!updated.has_value()) {
            return std::unexpected(VirtualCollectionError::CapacityExceeded);
        }
        scroll_offset_ = updated->scroll_offset;
        return VirtualScrollOutcome{index, updated->scroll_offset, updated->realized.first,
                                    updated->realized.last, updated->clamped};
    }

    [[nodiscard]] std::expected<void, VirtualCollectionError> refresh() override {
        const float offset = initialized_ ? scroll_offset_ : 0.0f;
        initialized_ = true;
        const auto updated = range_.update(offset, viewport_extent(container_));
        if (!updated.has_value()) {
            return std::unexpected(VirtualCollectionError::CapacityExceeded);
        }
        scroll_offset_ = updated->scroll_offset;
        return {};
    }

    [[nodiscard]] ui::Node* realized_node(std::size_t index) const noexcept override {
        return range_.realized_node(index);
    }

private:
    class Source final : public virt::VirtualDataSource {
    public:
        Source(const ui::Node& container, std::uint64_t& next_node_id)
            : container_(container),
              next_node_id_(&next_node_id) {}

        [[nodiscard]] std::size_t item_count() const override {
            return kItemCount;
        }
        [[nodiscard]] virt::VirtualItemId item_key(std::size_t index) const override {
            return virt::VirtualItemId{index};
        }
        [[nodiscard]] virt::VirtualItemMeasure
        measure(std::size_t, const virt::MeasureConstraints&) const override {
            return virt::VirtualItemMeasure{kItemMainExtent, container_.bounds().width};
        }
        void build(std::size_t index, ui::Node& out) override {
            configure_item(out, container_, index, (*next_node_id_)++);
        }

    private:
        const ui::Node& container_;
        std::uint64_t* next_node_id_ = nullptr;
    };

    [[nodiscard]] static virt::VirtualRange::Config range_config() {
        virt::VirtualRange::Config config;
        config.capacity = kItemCount;
        config.pool_cap = kListPoolCap;
        config.state_capacity = kStateCapacity;
        config.estimate_main = kItemMainExtent;
        config.gap = 0.0f;
        config.overscan_main = 0.0f;
        // The range places nodes viewport-relative and the container stays at scroll 0, so the
        // cross extent only feeds the realized node sizes; the content width is not used to scroll.
        config.cross_extent = 0.0f;
        config.horizontal = false;
        return config;
    }

    ui::Node& container_;
    Source source_;
    virt::VirtualRange range_;
    float scroll_offset_ = 0.0f;
    bool initialized_ = false;
};

// One grid collection. `VirtualGridRows` supplies row math (a row's extent is the max of its
// items); the harness owns the fixed-column window and realizes whole rows.
class ManifestGridCollection final : public VirtualCollection {
public:
    ManifestGridCollection(ui::Node& container, std::size_t columns, std::uint64_t& next_node_id)
        : container_(container),
          columns_(std::max<std::size_t>(columns, 1)),
          next_node_id_(&next_node_id),
          rows_(grid_config()) {
        (void)rows_.set_item_count(kItemCount);
    }

    [[nodiscard]] ui::Node& container() noexcept override {
        return container_;
    }
    [[nodiscard]] std::size_t item_count() const noexcept override {
        return rows_.item_count();
    }
    [[nodiscard]] float scroll_offset() const noexcept override {
        return scroll_offset_;
    }

    [[nodiscard]] std::expected<VirtualScrollOutcome, VirtualCollectionError>
    scroll_to_index(std::size_t index, virt::ScrollAlignment alignment) override {
        if (index >= item_count()) {
            return std::unexpected(VirtualCollectionError::IndexOutOfRange);
        }
        const std::size_t row_index = rows_.row_of(index);
        const float viewport = viewport_extent(container_);
        const float maximum = std::max(rows_.total_extent() - viewport, 0.0f);
        const float start = rows_.row_offset(row_index);
        const float extent = rows_.row_extent(row_index);
        const float end = start + extent;
        float offset = start;
        switch (alignment) {
        case virt::ScrollAlignment::Start:
            offset = start;
            break;
        case virt::ScrollAlignment::Center:
            offset = start - (viewport - extent) * 0.5f;
            break;
        case virt::ScrollAlignment::End:
            offset = end - viewport;
            break;
        case virt::ScrollAlignment::Nearest:
            if (start >= scroll_offset_ && end <= scroll_offset_ + viewport) {
                offset = scroll_offset_;
            } else if (start < scroll_offset_ || extent > viewport) {
                offset = start;
            } else {
                offset = end - viewport;
            }
            break;
        }
        const bool clamped = offset < 0.0f || offset > maximum;
        scroll_offset_ = std::clamp(offset, 0.0f, maximum);
        const auto realized = realize();
        if (!realized.has_value()) {
            return std::unexpected(realized.error());
        }
        const auto [first, last] = realized_bounds();
        return VirtualScrollOutcome{index, scroll_offset_, first, last, clamped};
    }

    [[nodiscard]] std::expected<void, VirtualCollectionError> refresh() override {
        return realize();
    }

    [[nodiscard]] ui::Node* realized_node(std::size_t index) const noexcept override {
        const auto found = realized_.find(index);
        return found == realized_.end() ? nullptr : found->second;
    }

private:
    [[nodiscard]] static virt::VirtualGridRows::Config grid_config() {
        virt::VirtualGridRows::Config config;
        config.columns = kGridColumns;
        config.capacity = kItemCount;
        config.fixed_row_extent = kItemMainExtent;
        config.gap = 0.0f;
        return config;
    }

    [[nodiscard]] std::pair<std::size_t, std::size_t> realized_bounds() const noexcept {
        if (realized_.empty()) {
            return {0, 0};
        }
        std::size_t first = std::numeric_limits<std::size_t>::max();
        std::size_t last = 0;
        for (const auto& [index, node] : realized_) {
            (void)node;
            first = std::min(first, index);
            last = std::max(last, index);
        }
        return {first, last + 1};
    }

    void release_outside(std::size_t first, std::size_t last) {
        for (auto it = realized_.begin(); it != realized_.end();) {
            if (it->first >= first && it->first < last) {
                ++it;
                continue;
            }
            (void)container_.remove_child(*it->second);
            it = realized_.erase(it);
        }
    }

    [[nodiscard]] float cell_width() const {
        return container_.bounds().width / static_cast<float>(columns_);
    }

    [[nodiscard]] std::expected<void, VirtualCollectionError> realize() {
        const float viewport = viewport_extent(container_);
        if (viewport <= 0.0f || rows_.row_count() == 0) {
            release_outside(0, 0);
            return {};
        }
        const virt::ItemGeometry::Range visible = rows_.visible_items(scroll_offset_, viewport);
        release_outside(visible.first, visible.last);
        if (visible.empty()) {
            return {};
        }
        const float width = cell_width();
        const std::size_t first_row = rows_.row_of(visible.first);
        const std::size_t last_row = rows_.row_of(visible.last - 1);
        for (std::size_t row_index = first_row; row_index <= last_row; ++row_index) {
            const virt::VirtualGridRows::Row row = rows_.row(row_index);
            for (std::size_t item = row.first; item < row.first + row.count; ++item) {
                if (realized_.find(item) != realized_.end()) {
                    continue;
                }
                auto node = std::make_unique<ui::Node>();
                configure_item(*node, container_, item, (*next_node_id_)++);
                realized_[item] = &container_.add_child(std::move(node));
            }
        }
        for (const auto& [index, node] : realized_) {
            const std::size_t row_index = rows_.row_of(index);
            const std::size_t column = rows_.column_of(index);
            const float y = rows_.row_offset(row_index) - scroll_offset_;
            node->set_bounds(ui::rhi::Rect{static_cast<float>(column) * width, y, width,
                                           rows_.row_extent(row_index)});
        }
        container_.set_content_size(container_.bounds().width, rows_.total_extent());
        return {};
    }

    ui::Node& container_;
    std::size_t columns_ = 1;
    std::uint64_t* next_node_id_ = nullptr;
    virt::VirtualGridRows rows_;
    std::unordered_map<std::size_t, ui::Node*> realized_;
    float scroll_offset_ = 0.0f;
};

} // namespace

ManifestVirtualScene::~ManifestVirtualScene() = default;

void ManifestVirtualScene::add_list(ui::Node& container) {
    auto collection = std::make_unique<ManifestListCollection>(container, next_node_id_);
    registry_.add(container, *collection);
    owned_.push_back(std::move(collection));
}

void ManifestVirtualScene::add_grid(ui::Node& container, std::size_t columns) {
    auto collection = std::make_unique<ManifestGridCollection>(container, columns, next_node_id_);
    registry_.add(container, *collection);
    owned_.push_back(std::move(collection));
}

void VirtualCollectionRegistry::add(ui::Node& container, VirtualCollection& collection) {
    by_node_[&container] = &collection;
    collections_.push_back(&collection);
}

VirtualCollection* VirtualCollectionRegistry::find(const ui::Node& container) const noexcept {
    const auto found = by_node_.find(&container);
    return found == by_node_.end() ? nullptr : found->second;
}

void VirtualCollectionRegistry::refresh_all() {
    for (VirtualCollection* collection : collections_) {
        (void)collection->refresh();
    }
}

} // namespace tessera::automation
