// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/automation/virtual_collection.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::automation {

// Fixed column count of a manifest `VirtualGrid` collection (documented in section 6.8).
inline constexpr std::size_t kManifestVirtualGridColumns = 4;

// The deterministic virtual collections behind the manifest scene's `VirtualList`/`VirtualGrid`
// nodes. `automation.manifest.json` describes structure, not data, so the harness gives each such
// node a fixed synthetic collection: 100 items, 16 px main extent (4 columns for a grid), keyed by
// the zero-based index, with every realized item node carrying the collection's `uiId` plus its
// item key. The values are documented in `docs/specs/headless-automation-v1.md` section 6.8.
class ManifestVirtualScene final {
public:
    ManifestVirtualScene() = default;
    ~ManifestVirtualScene();

    ManifestVirtualScene(const ManifestVirtualScene&) = delete;
    ManifestVirtualScene& operator=(const ManifestVirtualScene&) = delete;

    void add_list(ui::Node& container);
    void add_grid(ui::Node& container, std::size_t columns);

    [[nodiscard]] VirtualCollectionRegistry& registry() noexcept {
        return registry_;
    }

private:
    // Shared across every collection so a realized item id is unique in the scene and is never
    // reused after recycling (`automation-ids-v1.md` section 4).
    std::uint64_t next_node_id_ = 1'000'000;
    std::vector<std::unique_ptr<VirtualCollection>> owned_;
    VirtualCollectionRegistry registry_;
};

} // namespace tessera::automation
