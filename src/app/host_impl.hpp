// SPDX-License-Identifier: MIT

#pragma once

// The private state of AppHost, shared by the two translation units that implement it: the frame
// loop (host.cpp) and the embedded theme asset (host_theme.cpp). Not installed; only src/app
// includes it.

#include "tessera/app/host.hpp"
#include "tessera/rendering/damage.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace tessera::app {

struct AppHost::Impl {
    Impl(ui::Node& root, ui::style::CompiledStyleSheet sheet, runtime::RuntimeLoop& loop,
         HostConfig config);

    void apply_root_size();
    void rebuild_style_tree();
    void rebuild_cascade();
    // Rebuilds the cascade and style tree, drops the paint cache and marks `dirty` on the subtree.
    void apply_style_generation(ui::Dirty dirty_mask);
    void run_layout_pass();
    void register_embedded_stylesheets(std::span<const ui::style::EmbeddedStylesheet> sheets);
    void register_embedded_theme(std::span<const std::uint8_t> bytes);

    ui::Node& root;
    // The constructor sheet, kept so registration can rebuild the cascade; `sheet` is the active
    // one.
    ui::style::CompiledStyleSheet base_sheet;
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
    ui::layout::LayoutEngine engine;
    ui::layout::LayoutScheduler layout_scheduler{engine};
    ui::DirtyPass dirty;
    ui::layout::DamageTracker tracker;
    std::unique_ptr<rendering::DamageAccumulator> damage;
    ui::animation::AnimationRuntime animations;
    std::unique_ptr<ui::frame::FramePipeline> pipeline;
    ui::UpdateScheduler* scheduler = nullptr;
    bool reduced_motion = false;
    std::size_t rejected_stylesheet_count = 0;
    std::size_t rejected_theme_count = 0;

    ui::layout::LayoutResult last_layout;
    ui::layout::LayoutApplyResult last_applied;
    ui::frame::FrameRender last_render;
    // One-shot full-repaint request from request_full_repaint(). It is applied after the pipeline's
    // tick (which clears the accumulator), so it survives the tick it was requested before.
    bool full_repaint_pending = false;
};

} // namespace tessera::app
