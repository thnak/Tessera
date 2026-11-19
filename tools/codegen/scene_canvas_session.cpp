// SPDX-License-Identifier: MIT

#include "diagnostic_record.hpp"
#include "scene_canvas_session.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;
namespace hal = tessera::hal;

// The domains a frame owes. Style is what `:hover`/`:active` sets through
// `Node::set_pseudo_state`; Layout/Paint/Render are what the post-dispatch rebuild sets. Any of
// them means the painted pixels could differ from the last frame's.
constexpr ui::Dirty kFrameDomains =
    ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render;

} // namespace

CanvasSceneSession::CanvasSceneSession()
    : sheet_(compile_preview_stylesheet({}, preview_media_context(320.0f, 240.0f, 1.0f))) {}

CanvasSceneSession::~CanvasSceneSession() = default;

std::string CanvasSceneSession::start(const CanvasSessionSource& source) {
    // One file is a project of one: the same code runs, so a single-file caller cannot see a
    // different session from the studio's.
    const std::string file(source.file.empty() ? std::string_view("ui/Preview.tsx") : source.file);
    const std::array<ProjectFile, 1> sources{{{file, source.text}}};
    const std::array<ProjectFile, 1> sheets{{{source.stylesheet_file, source.stylesheet}}};
    return start(ProjectInput{sources, sheets, file}, source);
}

std::string CanvasSceneSession::start(const ProjectInput& project,
                                      const CanvasSessionSource& source) {
    const std::string component(source.component);
    const float dpi = source.dpi_scale > 0.0f ? source.dpi_scale : 1.0f;

    // A Run of the identical source is a repaint, not a re-evaluation. The page hands the editor's
    // text to every frame, so re-evaluating here would drop the hover bits and the state cells
    // between two frames of the same program. The stylesheet is part of the identity: a Run that
    // changed only the CSS must re-resolve style, or the canvas would keep painting the previous
    // sheet while the diagnostics area reported the new one.
    //
    // **The viewport and the posture are NOT part of it** (issue #436). They are layout inputs, so
    // a Run that only moved the device preset re-lays the RUNNING tree out in place - one pass, no
    // evaluation - which is what keeps hover/focus/active and the state cells across a device or
    // orientation change. Before this, width/height were in `reused_ok`, so a toolbar change
    // re-evaluated the source and lost exactly that.
    if (session_ != nullptr && reused_ok(project, component, source.measurer)) {
        reused_ = true;
        (void)apply_layout_inputs(source.width, source.height, dpi, source.posture, source.insets,
                                  source.cutouts);
        return {};
    }

    // **The new source is evaluated before the running evaluation is touched.** That ordering is
    // what makes a *live* edit safe (issue #444): a developer typing in the editor passes through
    // sources that do not parse (`if (count > 0) {` with no closing brace yet), and each of those
    // must cost a diagnostic only - never the running tree, its state cells, the sheet its boxes
    // were laid out with, or the last painted frame. A failed evaluation therefore leaves
    // `session_`, `evaluated_`, `sheet_`, `css_diagnostics_` and the recorded source tuple exactly
    // as they were, so the session still owes no frame (`needs_frame()` is false), the page paints
    // nothing, and the pixels already on the canvas stay; the next source that does evaluate
    // captures the state from the tree that survived, which is why a counter typed through a
    // transient syntax error still holds its count.
    //
    // The author sheet (issue #390) is compiled *after* the evaluation succeeds, not before: it and
    // the tree it styles have to move together, because a sheet swapped in over a tree laid out for
    // the previous one would paint the old boxes through the new cascade. Nothing is lost from
    // #390 by that ordering -- the moment the TSX parses, the CSS diagnostics the author typed are
    // reported with the frame that paints them, which is #390's acceptance case. A CSS problem
    // typed while the TSX is mid-word is reported as soon as the source evaluates again, and a
    // developer never has to press Restart to find out.
    auto evaluated = evaluate_project(project, source.salt, component);
    if (!evaluated) {
        error_ = evaluated.error().detail;
        if (error_.empty()) {
            error_ = "the source produced no component";
        }
        reused_ = false;
        // `fresh_` is deliberately NOT cleared: a frame a previous successful edit already owed is
        // still owed, and clearing it here would leave that edit's tree unpainted forever.
        return error_;
    }

    // Capture before the old tree dies: `capture()` reads the scope the tree was built from, and
    // the tree borrows strings from `evaluated_`, which is about to be replaced.
    CarriedState carried;
    if (session_ != nullptr) {
        carried = session_->capture();
    }
    session_.reset();
    error_.clear();
    reused_ = false;
    fresh_ = false;
    // The sheets the program imports and their diagnostics, from one preparation: the diagnostics
    // describe the sheets the tree below is laid out with, so the pane can never report one sheet's
    // problems beside another sheet's pixels. They are PREPARED once and resolved against the
    // viewport (issue #436), and the prepared form goes into the session so a later viewport change
    // can re-resolve the same data instead of re-parsing CSS.
    const ui::style::MediaContext media = preview_media_context(source.width, source.height, dpi);
    PreviewSheets prepared_sheets = prepare_preview_sheets(evaluated->sheets);
    sheet_ = prepared_sheets.prepared.resolve(media);
    css_diagnostics_ = std::move(prepared_sheets.diagnostics);
    std::optional<PreparedPreviewSheets> prepared(std::move(prepared_sheets.prepared));
    evaluated_ = std::move(*evaluated);

    // Every component id in the new evaluation, so a carried cell whose component disappeared is
    // reported as dropped instead of being offered to whatever component took its place.
    std::vector<std::string> live_ids;
    live_ids.reserve(evaluated_.module.components.size());
    for (const auto& candidate : evaluated_.module.components) {
        live_ids.push_back(candidate.component_id);
    }

    PreviewSession::Options options;
    options.width = source.width;
    options.height = source.height;
    options.dpi_scale = dpi;
    options.posture = source.posture;
    options.insets = source.insets;
    options.cutouts = source.cutouts;
    options.carried = &carried;
    options.live_component_ids = &live_ids;
    options.measurer = source.measurer;
    options.overrides = &overrides_;
    options.prepared = std::move(prepared);
    options.media = media;

    session_ = std::make_unique<PreviewSession>(evaluated_.module, evaluated_.selected(), sheet_,
                                                std::move(options));
    if (!session_->built()) {
        error_ = "TESSERA_SCENE_UNSUPPORTED: the source's tree cannot be previewed";
        return error_;
    }
    carry_report_ = session_->carry_report();
    metrics_ = session_->metrics();
    // The compiler's own answer to "which components can this entry preview": the ones declared in
    // the entry file, not the building blocks it imports.
    entry_components_.clear();
    const std::string entry_path = normalize_path(project.entry);
    for (const auto& candidate : evaluated_.module.components) {
        if (normalize_path(candidate.source.file) == entry_path) {
            entry_components_.push_back(candidate.name);
        }
    }
    component_name_ = evaluated_.selected().name;
    project_ = OwnedProject::of(project);
    component_ = component;
    width_ = source.width;
    height_ = source.height;
    dpi_scale_ = dpi;
    posture_ = source.posture;
    insets_ = source.insets;
    cutouts_ = source.cutouts;
    measurer_ = source.measurer;
    fresh_ = true;
    return error_;
}

bool CanvasSceneSession::reused_ok(
    const ProjectInput& project, const std::string& component,
    const std::shared_ptr<const text::TextMeasurer>& measurer) const {
    // The measurer is part of the identity: a Run that arrives with font bytes when the running
    // evaluation had none must re-evaluate, because every text box it laid out was sized by the
    // font-free stub.
    //
    // The viewport and the DPI scale are deliberately NOT here (issue #436): they are layout inputs
    // the running tree can be re-laid out for, not properties of the source. Keeping them in this
    // tuple is what made a device-preset change re-evaluate - and lose hover, focus and active.
    return project_.same_as(project) && component == component_ &&
           measurer.get() == measurer_.get();
}

bool CanvasSceneSession::set_posture(hal::FoldPosture posture) {
    return apply_layout_inputs(width_, height_, dpi_scale_, posture, insets_, cutouts_);
}

bool CanvasSceneSession::set_viewport(float width, float height, float dpi_scale) {
    return apply_layout_inputs(width, height, dpi_scale, posture_, insets_, cutouts_);
}

bool CanvasSceneSession::set_safe_area(const hal::WindowInsets& insets,
                                       const hal::CutoutInfo& cutouts) {
    return apply_layout_inputs(width_, height_, dpi_scale_, posture_, insets, cutouts);
}

bool CanvasSceneSession::apply_layout_inputs(float width, float height, float dpi_scale,
                                             hal::FoldPosture posture) {
    return apply_layout_inputs(width, height, dpi_scale, posture, insets_, cutouts_);
}

bool CanvasSceneSession::apply_layout_inputs(float width, float height, float dpi_scale,
                                             hal::FoldPosture posture,
                                             const hal::WindowInsets& insets,
                                             const hal::CutoutInfo& cutouts) {
    if (!started()) {
        return false;
    }
    const float dpi = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    // Every layout input is compared, the skin's safe area included: a skin whose size happens to
    // match the previous one but whose insets differ is still a different device, and skipping the
    // pass would leave the content padded for the device that is no longer on screen.
    if (width == width_ && height == height_ && dpi == dpi_scale_ && posture == posture_ &&
        same_insets(insets, insets_) && same_cutouts(cutouts, cutouts_)) {
        return false;
    }
    // One pass for every input, inside this call: the boxes (and, when the size crosses a
    // breakpoint, the cascade) are already the new ones when it returns, so the frame the page
    // draws next is the FIRST to show the new device - no frame of stale padding or stale
    // breakpoint (issues #408, #436 and #171).
    if (!session_->relayout_at(width, height, dpi, posture, insets, cutouts)) {
        return false;
    }
    width_ = width;
    height_ = height;
    dpi_scale_ = dpi;
    posture_ = posture;
    insets_ = insets;
    cutouts_ = cutouts;
    metrics_ = session_->metrics();
    return true;
}

namespace {

// The focusable, visible nodes in tree order, which is the sequential tab order of a scene the
// preview builds (it sets no positive `tabIndex`). Used only to know where the order ends.
void collect_focusable(const ui::Node& node, std::vector<const ui::Node*>& out) {
    if (!node.visible()) {
        return;
    }
    if (node.focusable()) {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_focusable(*child, out);
    }
}

} // namespace

void CanvasSceneSession::fill_focus(CanvasPointerResult& result) const {
    if (const ui::Node* focused = session_->router().focus_manager().focused()) {
        result.focused_id = focused->automation_id();
        result.focus_visible =
            ui::style::has_state(focused->pseudo_state(), ui::style::PseudoState::FocusVisible);
    }
}

CanvasPointerResult CanvasSceneSession::key(PreviewInput::Kind kind, std::uint32_t key,
                                            std::uint32_t modifiers, bool repeat) {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    if (kind != PreviewInput::Kind::KeyDown && kind != PreviewInput::Kind::KeyUp) {
        return result;
    }
    ui::InputRouter& router = session_->router();
    ui::FocusManager& focus = router.focus_manager();
    const ui::Node* const before = focus.focused();

    // Tab at the end of the order lets go instead of wrapping. The router wraps (a native window
    // has nowhere else to go), but a canvas sits inside a page, and a wrap here is a keyboard trap.
    const bool plain_tab = key == ui::key::kTab &&
                           (modifiers & (static_cast<std::uint32_t>(ui::Modifier::Control) |
                                         static_cast<std::uint32_t>(ui::Modifier::Alt) |
                                         static_cast<std::uint32_t>(ui::Modifier::Meta))) == 0U;
    if (plain_tab) {
        const bool backward = (modifiers & static_cast<std::uint32_t>(ui::Modifier::Shift)) != 0U;
        std::vector<const ui::Node*> order;
        collect_focusable(session_->root(), order);
        const bool at_end =
            order.empty() ||
            (before != nullptr && (backward ? before == order.front() : before == order.back()));
        if (at_end) {
            if (kind == PreviewInput::Kind::KeyDown && before != nullptr) {
                focus.clear_focus();
            }
            result.left = true;
            result.focus_moved = before != nullptr && kind == PreviewInput::Kind::KeyDown;
            result.changed = needs_frame();
            return result;
        }
    }

    PreviewInput input;
    input.kind = kind;
    input.key = key;
    input.modifiers = modifiers;
    input.repeat = repeat;
    result.dispatched = session_->dispatch(input);
    result.ran = session_->last_ran();
    result.unrun = session_->last_unrun();
    fill_focus(result);
    if (const ui::Node* hover = session_->router().hovered()) {
        result.hovered_id = hover->automation_id();
    }
    result.focus_moved = focus.focused() != before;
    // Consumed when the scene did something with it: it reached a focused node, or it moved focus.
    // A key with no focus and no traversal falls through to the page untouched.
    result.handled = result.dispatched || result.focus_moved;
    result.changed = needs_frame();
    return result;
}

CanvasPointerResult CanvasSceneSession::blur() {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    ui::FocusManager& focus = session_->router().focus_manager();
    const bool had_focus = focus.focused() != nullptr;
    focus.clear_focus();
    result.focus_moved = had_focus;
    result.changed = needs_frame();
    return result;
}

CanvasPointerResult CanvasSceneSession::pointer(PreviewInput::Kind kind, float x, float y,
                                                std::uint32_t button) {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    // This slice wires the primary button only: a secondary press has no `CanvasAction` yet, so
    // dispatching it as a left press would run an `onClick` the user never asked for. A move
    // carries no button and is always dispatched.
    const bool contact =
        kind == PreviewInput::Kind::PointerDown || kind == PreviewInput::Kind::PointerUp;
    if (contact && button != 0) {
        return result;
    }

    PreviewInput input;
    input.kind = kind;
    input.x = x;
    input.y = y;
    result.dispatched = session_->dispatch(input);
    result.ran = session_->last_ran();
    result.unrun = session_->last_unrun();

    if (const ui::Node* hover = session_->router().hovered()) {
        result.hovered_id = hover->automation_id();
    }
    if (const ui::Node* active = session_->router().active()) {
        result.active_id = active->automation_id();
    }
    result.target_id = result.dispatched ? result.hovered_id : 0;
    fill_focus(result);
    // "A frame is owed" is the scene's own answer, never a guess: a dispatch that changed no
    // style/layout/paint domain costs no frame.
    result.changed = needs_frame();
    return result;
}

CanvasPickResult CanvasSceneSession::pick(float x, float y) {
    CanvasPickResult result;
    if (!started()) {
        return result;
    }
    // The router's own hit test, on the live tree: `Node::hit_test_deep` is the function
    // `ui::InputRouter` calls to decide a pointer target, so the node reported here is exactly the
    // node a press at the same point would reach. It is non-const because it walks mutable
    // children, but it writes nothing - which is why `pick()` is a query even though it cannot be
    // `const`.
    ui::Node* hit = session_->root().hit_test_deep(x, y);
    if (hit == nullptr) {
        return result;
    }
    result.found = true;
    result.automation_id = hit->automation_id();
    result.element_kind = hit->element_kind();
    result.box = hit->world_bounds();
    return result;
}

bool CanvasSceneSession::needs_frame() const {
    if (!started()) {
        return false;
    }
    if (fresh_) {
        return true;
    }
    return ui::has_any(session_->root().subtree_dirty(), kFrameDomains);
}

SceneFrameResult CanvasSceneSession::paint(rhi::Device& device, rhi::RenderTarget& target,
                                           const rhi::RenderTargetDesc& desc,
                                           const rhi::PipelineDesc& pipeline,
                                           PreviewTextStack* text) {
    SceneFrameResult result;
    if (!started()) {
        result.error = error_.empty() ? "the session has no scene to paint" : error_;
        return result;
    }
    result =
        scene_frame_paint_tree(session_->root(), sheet(), device, target, desc, pipeline, text);
    if (!result.error.empty()) {
        return result;
    }
    // The frame consumed the dirty domains. A static scene therefore has none set, which is what
    // makes `needs_frame()` answer false and the caller draw nothing until the next event.
    dirty_.clear(session_->root(), kFrameDomains);
    fresh_ = false;
    ++frames_painted_;
    return result;
}

const ui::Node* CanvasSceneSession::root() const noexcept {
    return session_ != nullptr ? &session_->root() : nullptr;
}

const ui::InputRouter* CanvasSceneSession::router() const noexcept {
    return session_ != nullptr ? &session_->router() : nullptr;
}

} // namespace tessera::compiler::codegen::cli
