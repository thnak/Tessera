// SPDX-License-Identifier: MIT

#include "scene_canvas_session.hpp"
#include "tessera/ui/dirty.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

// The domains a frame owes. Style is what `:hover`/`:active` sets through
// `Node::set_pseudo_state`; Layout/Paint/Render are what the post-dispatch rebuild sets. Any of
// them means the painted pixels could differ from the last frame's.
constexpr ui::Dirty kFrameDomains =
    ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render;

} // namespace

CanvasSceneSession::CanvasSceneSession()
    : sheet_(ui::style::compile_stylesheet(preview_stylesheet())) {}

CanvasSceneSession::~CanvasSceneSession() = default;

std::string CanvasSceneSession::start(const CanvasSessionSource& source) {
    const std::string text(source.text);
    const std::string file = normalize_path(source.file.empty() ? "ui/Preview.tsx" : source.file);
    const std::string component(source.component);
    const float dpi = source.dpi_scale > 0.0f ? source.dpi_scale : 1.0f;

    // A Run of the identical source is a repaint, not a re-evaluation. The page hands the editor's
    // text to every frame, so re-evaluating here would drop the hover bits and the state cells
    // between two frames of the same program.
    if (session_ != nullptr && reused_ok(text, file, component, source.width, source.height, dpi)) {
        reused_ = true;
        return {};
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

    auto evaluated = evaluate_source(text, file, source.salt, component);
    if (!evaluated) {
        evaluated_ = EvaluatedScene{};
        error_ = evaluated.error().detail;
        if (error_.empty()) {
            error_ = "the source produced no component";
        }
        return error_;
    }
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
    options.carried = &carried;
    options.live_component_ids = &live_ids;

    session_ =
        std::make_unique<PreviewSession>(evaluated_.module, evaluated_.selected(), sheet_, options);
    if (!session_->built()) {
        error_ = "TESSERA_SCENE_UNSUPPORTED: the source's tree cannot be previewed";
        return error_;
    }
    text_ = text;
    file_ = file;
    component_ = component;
    width_ = source.width;
    height_ = source.height;
    dpi_scale_ = dpi;
    fresh_ = true;
    return error_;
}

bool CanvasSceneSession::reused_ok(const std::string& text, const std::string& file,
                                   const std::string& component, float width, float height,
                                   float dpi) const {
    return text == text_ && file == file_ && component == component_ && width == width_ &&
           height == height_ && dpi == dpi_scale_;
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
    // "A frame is owed" is the scene's own answer, never a guess: a dispatch that changed no
    // style/layout/paint domain costs no frame.
    result.changed = needs_frame();
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
                                           const rhi::PipelineDesc& pipeline) {
    SceneFrameResult result;
    if (!started()) {
        result.error = error_.empty() ? "the session has no scene to paint" : error_;
        return result;
    }
    result = scene_frame_paint_tree(session_->root(), sheet_, device, target, desc, pipeline);
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
