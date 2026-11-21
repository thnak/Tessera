// SPDX-License-Identifier: MIT

#pragma once

// The private state of AppHost, shared by the two translation units that implement it: the frame
// loop (host.cpp) and the embedded theme asset (host_theme.cpp). Not installed; only src/app
// includes it.

#include "adaptive_quality.hpp"
#include "memo_style_provider.hpp"
#include "quality_stage.hpp"
#include "safe_area_state.hpp"
#include "tessera/app/host.hpp"
#include "tessera/rendering/damage.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/layout/layout_settle.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tessera::app {

namespace detail {

// The physical extent of a logical size at a DPI scale: the damage target's rectangle. Shared by
// the constructor and by a size change, so a target and a viewport cannot round differently.
[[nodiscard]] rendering::rhi::Extent2D extent_for(float width, float height, float dpi_scale);

// Marks every node of `node`'s subtree: what a new style generation or a viewport change implies.
// One implementation, shared by the cascade rebuild (host.cpp) and the media half of a resize
// (host_viewport.cpp, issue #436).
void mark_subtree(ui::Node& node, ui::Dirty dirty);

} // namespace detail

// The media context of a window `width` x `height` at `dpi_scale` (issue #436): the logical client
// area as the viewport, the DPI scale as CSS dpi (96 dpi at scale 1), and `appearance` folded in by
// the one HAL-to-CSS mapping. Both constructors and every size change go through it, so the
// derivation exists once (`host_viewport.cpp`).
[[nodiscard]] ui::style::MediaContext
window_media_context(float width, float height, float dpi_scale,
                     const hal::AppearancePreference& appearance) noexcept;

struct AppHost::Impl {
    Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
         HostConfig config);

    void apply_root_size();
    // One window-rectangle change, whoever reported it: the driver's `resize()` and a
    // `set_window_metrics()` that names a different extent both land here (issue #436).
    void apply_size(float width, float height, float dpi_scale);
    // The media half of `apply_size()`: moves `media_context` and re-resolves the cascade only when
    // a `@media` answer can actually change. A resize that crosses no breakpoint marks `dirty_mask`
    // and stops there.
    void apply_viewport(ui::Dirty dirty_mask);
    [[nodiscard]] bool media_answer_can_change(const ui::style::MediaContext& before,
                                               const ui::style::MediaContext& after) const;
    void rebuild_style_tree();
    // Brings the selector tree up to date for one restyle. Returns true when the previous
    // generation was kept and only the Style-dirty subtrees were invalidated (issue #711): the
    // shape is unchanged and no selector reads a sibling. Otherwise it rebuilds the tree.
    [[nodiscard]] bool prepare_style_tree();
    // True once a restyle has resolved every node of the current tree, so its memo can be reused.
    bool style_tree_populated = false;
    void rebuild_cascade();
    // Rebuilds the cascade and style tree, drops the paint cache and marks `dirty` on the subtree.
    void apply_style_generation(ui::Dirty dirty_mask);
    void run_layout_pass(ui::paint::StyleProvider& styles);
    // The one restyle-and-layout sequence a frame and the settle loop share (issue #693 decision
    // 1): rebuild the selector tree, drop the paint cache, run style and layout, sync the scene
    // style and sample the animations that started. A settle round that only re-laid-out would
    // paint a stale style generation or replay a stale paint cache.
    void restyle_and_layout(ui::frame::FrameTick& tick, ui::paint::SceneStyleSyncResult& sync,
                            bool& style_rebuilt);
    // Publishes the layout the pass just applied and runs the settle loop (ADR-041, issue #693).
    // Called right after `run_layout_pass` in every path that lays out.
    void settle_layout();
    // The settle loop's relayout hook: everything `restyle_and_layout` does, plus the publish.
    static void settle_relayout(void* context);
    static void settle_wake(void* context);
    // The window minus the safe-area insets, and the root's text direction, for overlay placement.
    [[nodiscard]] rendering::rhi::Rect overlay_viewport() const;
    [[nodiscard]] bool root_is_rtl() const;
    // Resolves one BackRequested (host_back.cpp); always consumes it.
    bool handle_back_request();
    void register_embedded_stylesheets(std::span<const ui::style::EmbeddedStylesheet> sheets);
    void register_embedded_theme(std::span<const std::uint8_t> bytes);

    // The adaptive-quality stage's callbacks. Static members rather than file-local functions
    // because `Impl` is a private nested type, so a free function cannot name it; defined inline
    // here because they are three-line accessors and host.cpp is already at its size budget.
    static bool scene_has_blurred_shadow_callback(const void* context) noexcept;
    static bool non_essential_animations_callback(const void* context) noexcept;
    static void set_reduce_effects_callback(void* context, bool reduced);
    static void set_animations_paused_callback(void* context, bool paused);

    ui::Node& root;
    SafeAreaState safe_area;
    // Created by AppHost::overlays(); destroyed before the scene root it borrows.
    std::unique_ptr<ui::OverlayHost> overlay_host;
    // An Escape press the overlays consumed; its release is swallowed too.
    bool escape_swallowed = false;
    BackHandler back_handler = nullptr;
    void* back_handler_user = nullptr;
    BackDispatch last_back = BackDispatch::None;
    std::size_t unhandled_backs = 0;
    // The constructor sheet, kept so registration can rebuild the cascade; `sheet` is the active
    // one.
    // The constructor sheet, kept so registration can rebuild the cascade; `sheet` is the active
    // one. When the caller supplied the re-resolvable prepared form (issue #187), `base_prepared`
    // holds it and every rebuild re-resolves it against the current media context; a caller that
    // supplied a compiled sheet leaves it null and a color-scheme change cannot re-resolve.
    ui::style::CompiledStyleSheet base_sheet;
    std::optional<ui::style::PreparedStyleSheet> base_prepared;
    ui::style::Origin base_origin = ui::style::Origin::Author;
    ui::style::CompiledStyleSheet sheet;
    // The embedded sheets as loaded at registration, so a theme switch can rebuild the cascade
    // without the caller re-registering them.
    std::vector<ui::style::PreparedStyleSheet> embedded_sheets;
    // The installed theme asset (the built-in `default` until a project blob is registered) and the
    // theme currently injected.
    ui::style::ThemeAsset theme_asset;
    std::string active_theme_id = std::string(ui::style::kDefaultThemeId);
    runtime::RuntimeLoop& loop;
    HostConfig config;

    // Host-owned storage for the root's inline `width`/`height` text (the scene borrows it).
    std::array<char, 32> root_width_text_{};
    std::array<char, 32> root_height_text_{};

    std::unique_ptr<ui::style::SceneStyleTree> tree;
    std::unique_ptr<ui::paint::SceneStyleProvider> provider;
    // One resolve per node per restyle, shared by layout input and the style sync (#711).
    MemoStyleProvider restyle_memo;
    ui::layout::LayoutEngine engine;
    ui::layout::LayoutScheduler layout_scheduler{engine};
    ui::DirtyPass dirty;
    ui::layout::DamageTracker tracker;
    std::unique_ptr<rendering::DamageAccumulator> damage;
    ui::animation::AnimationRuntime animations;
    std::unique_ptr<ui::frame::FramePipeline> pipeline;
    ui::UpdateScheduler* scheduler = nullptr;
    bool reduced_motion = false;
    // The OS appearance snapshot in force (issue #187) and the media context it produces. The
    // stylesheet's `@media (prefers-color-scheme/...)` rules resolved against `media_context`; a
    // color-scheme change rebuilds the cascade through `apply_style_generation`, the same path a
    // theme switch takes, so the stylesheet never grows a second "theme changed" mechanism.
    hal::AppearancePreference appearance{};
    ui::style::MediaContext media_context;
    // Cascade generations: 1 after construction, +1 for every rebuild of the resolved cascade
    // (`rebuild_cascade`). It is how "a resize that crosses no breakpoint does not rebuild the
    // cascade" is asserted in a test instead of inferred from a picture (issue #436).
    std::uint64_t cascade_generation = 0;
    std::size_t rejected_stylesheet_count = 0;
    std::size_t rejected_theme_count = 0;

    ui::layout::LayoutResult last_layout;
    ui::layout::LayoutApplyResult last_applied;
    // The ADR-041 settle loop (issue #693): the layout read model, the container-query tracker and
    // the app observations, in one shared component the Studio canvas session also uses. It costs
    // one boolean test per frame when the sheet has no `@container` and nothing observes.
    ui::layout::LayoutSettle settle;
    ui::frame::FrameRender last_render;
    // Adaptive quality (docs/design/performance.md section 6): the deterministic controller is fed
    // one measured frame time per presented frame by observe_frame_time_ms(); the diagnostics
    // object keeps its latest decision and counts changes. Both are per host (not per pipeline), so
    // the quality ladder survives an AppHost::resize(), which only re-creates the pipeline. The
    // ladder itself, its recording and its application rules live in detail::QualityStage.
    detail::QualityStage quality;
    runtime::FrameDiagnostics frame_diagnostics;
    // One-shot full-repaint request from request_full_repaint(). It is applied after the pipeline's
    // tick (which clears the accumulator), so it survives the tick it was requested before.
    bool full_repaint_pending = false;
};

} // namespace tessera::app
