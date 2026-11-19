// SPDX-License-Identifier: MIT
//
// `CanvasSceneSession::platform_event` (designer-protocol-v1 section 5.1, issue #446): the canvas
// session's adapter onto the shared `PlatformEventDriver`.

#include "scene_canvas_session.hpp"

#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

// A layout change goes through the canvas session, not the bare tree: the session keeps its own
// copy of the viewport and the posture so a later `start()` re-lays the tree out at what the
// device last had, and writing around it would revert an orientation swap on the next apply.
class CanvasTarget final : public PlatformTarget {
public:
    CanvasTarget(CanvasSceneSession& owner, PreviewSession& session)
        : owner_(owner),
          session_(session) {}
    bool set_viewport(float width, float height, float dpi_scale) override {
        return owner_.set_viewport(width, height, dpi_scale);
    }
    bool set_posture(hal::FoldPosture posture) override {
        return owner_.set_posture(posture);
    }
    const hal::WindowMetrics& metrics() const override {
        return owner_.metrics();
    }
    CarriedState capture() const override {
        return session_.capture();
    }
    ui::InputDispatchResult dispatch_input(const ui::InputEvent& event) override {
        return session_.router().dispatch(session_.root(), event);
    }

private:
    CanvasSceneSession& owner_;
    PreviewSession& session_;
};

} // namespace

PlatformReply CanvasSceneSession::platform_event(const PlatformEvent& event) {
    if (session_ == nullptr || !session_->built()) {
        PlatformReply none;
        none.kind = event.kind;
        none.ok = false;
        none.error = "TESSERA_PLATFORM_NO_SESSION: no canvas session: call "
                     "tessera_canvas_apply first";
        return none;
    }
    CanvasTarget target(*this, *session_);
    PlatformReply reply = platform_.deliver(target, event);
    if (reply.relaunch) {
        // The process is gone and comes back: the tree dies here and the page's next apply is a
        // cold start. The cells the Suspend saved ride into it; an unhandled Back saved none.
        relaunch_carry_ = reply.restore && platform_.saved() ? *platform_.saved() : CarriedState{};
        platform_.cold_start();
        session_.reset();
        fresh_ = false;
    }
    return reply;
}

} // namespace tessera::compiler::codegen::cli
