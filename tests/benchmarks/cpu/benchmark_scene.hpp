// SPDX-License-Identifier: MIT
//
// The reference scene for the CPU benchmark (issue #241): a deterministic dashboard of exactly
// 12 000 scene nodes built from a fixed generator (no seed-dependent output, no clocks, no fonts).
// It mixes what a real application tree mixes: containers, text runs, raster images, scrolling
// regions and a virtualized list whose realized window is the only part in the scene.
//
// The scene is built once per measured run so every run sees a cold style tree, a cold layout and
// a cold paint cache. Node ids are handed out in build order, so two runs produce the same ids;
// the only variation is explicit scenario edits made by the benchmark driver.

#pragma once

#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace tessera::ui::virtualization {
class VirtualDataSource;
class VirtualRange;
} // namespace tessera::ui::virtualization

namespace tessera::benchmark {

// Logical (CSS pixel) viewport; the host lays the root out at this size.
inline constexpr float kBenchmarkWidth = 1280.0f;
inline constexpr float kBenchmarkHeight = 800.0f;

// The committed budget and the docs report this exact node count. The builder pads the tree to it,
// so a graph change that adds or drops nodes fails loudly here instead of silently moving the
// measurement to a different tree size.
inline constexpr std::size_t kBenchmarkNodes = 12000;

// Nodes the driver edits for the two incremental scenarios. Both are ordinary dashboard content,
// not special-case nodes: a card title (text change) and a card container (size change).
// `glass_bar` is the fixed top bar, which the glass scenario gives a `backdrop-filter` (glass-v1
// section 9); it is an existing node, so the reference tree stays at exactly `kBenchmarkNodes`
// nodes.
struct SceneTargets {
    ui::Node* text_node = nullptr;
    ui::Node* container_node = nullptr;
    ui::Node* glass_bar = nullptr;
    std::size_t nodes = 0;
};

// Builds the reference scene under `root`. `root` is the AppHost scene root and stays borrowed.
// The returned targets point into the tree and stay valid for the builder's lifetime.
class SceneBuilder final {
public:
    SceneBuilder();
    ~SceneBuilder();

    SceneBuilder(const SceneBuilder&) = delete;
    SceneBuilder& operator=(const SceneBuilder&) = delete;

    [[nodiscard]] SceneTargets build(ui::Node& root);
    [[nodiscard]] std::size_t nodes() const noexcept {
        return nodes_;
    }

private:
    struct VirtualSource;

    ui::Node& node(ui::Node& parent, const char* kind, const char* class_name);
    ui::Node& text(ui::Node& parent, const char* kind, const char* class_name, std::string text);
    ui::Node& image(ui::Node& parent, const char* class_name, std::string_view asset);
    void add_row(ui::Node& parent, const char* row_class, const char* cell_class, std::size_t cells,
                 std::uint64_t row_number);
    void add_item(ui::Node& item, std::size_t index);
    void pad_to_target(ui::Node& parent);
    [[nodiscard]] std::uint64_t next_random();

    std::unique_ptr<VirtualSource> source_;
    std::unique_ptr<ui::virtualization::VirtualRange> range_;
    std::uint64_t next_id_ = 2;
    std::size_t nodes_ = 1;
    std::uint64_t random_state_ = 0x9E3779B97F4A7C15ULL;
};

// The stylesheet the scene is built for. Pure CSS: class selectors only, no transforms, no fonts.
[[nodiscard]] ui::style::CompiledStyleSheet compile_benchmark_stylesheet();

} // namespace tessera::benchmark
