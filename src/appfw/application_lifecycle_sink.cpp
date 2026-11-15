// SPDX-License-Identifier: MIT
//
// The one place layer 0's window driver meets layer 1's lifecycle state machine. `WindowHost`
// forwards the lifecycle events it already polls (`app::LifecycleSink`); this adapter maps each
// `hal::WindowEvent` onto the portable vocabulary and feeds `Application::dispatch`.
//
// Two rules make the forwarding trustworthy:
//
//   * a forwarded event that carries no lifecycle transition is dropped, not translated into a
//     guessed one, so a resize or a display change can never move the app state;
//   * ordering is the driver's, which is the poll order inside one iteration, so the sequence a
//     service observes is the sequence the platform produced.

#include "tessera/appfw/application.hpp"
#include "tessera/appfw/lifecycle.hpp"

#include <memory>

namespace tessera::appfw {
namespace {

class ApplicationLifecycleSink final : public app::LifecycleSink {
public:
    explicit ApplicationLifecycleSink(Application& application)
        : application_(&application) {}

    void on_window_lifecycle(const hal::WindowEvent& event) override {
        LifecycleSignal signal{};
        if (!lifecycle_signal_for(event.type, signal)) {
            return;
        }
        // A metrics-carrying lifecycle event (a restore, a minimize) is also the platform's newest
        // window snapshot, so the aggregate is updated from it: a `Restored` event whose metrics
        // still say "occluded" leaves the window occluded, which is what the driver's own pause
        // state does with the same pair (src/app/window_lifecycle.cpp).
        if (signal == LifecycleSignal::Restore) {
            (void)application_->apply_window_snapshot(event.metrics.focused, event.metrics.occluded,
                                                      event.metrics.minimized);
            return;
        }
        (void)application_->dispatch(LifecycleEvent{signal});
    }

private:
    Application* application_ = nullptr;
};

} // namespace

std::unique_ptr<app::LifecycleSink> Application::make_lifecycle_sink() {
    return std::make_unique<ApplicationLifecycleSink>(*this);
}

} // namespace tessera::appfw
