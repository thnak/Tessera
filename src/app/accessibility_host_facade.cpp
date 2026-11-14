// SPDX-License-Identifier: MIT
//
// The public face of `AccessibilityHost` (issues #185 and #186): thin forwarding over the private
// `Impl` in accessibility_host.cpp. Kept in its own translation unit so the lifecycle/UIA
// implementation and this facade each stay well under the file-size budget.

#include "accessibility_host.hpp"
#include "accessibility_host_impl.hpp"

#include <utility>

namespace tessera::app {

AccessibilityHost::AccessibilityHost()
    : impl_(std::make_unique<Impl>()) {}

AccessibilityHost::~AccessibilityHost() {
    stop();
}

bool AccessibilityHost::start(hal::Window& window, AccessibilitySnapshotSource source,
                              AccessibilityActionHandler handler, AccessibilityHostConfig config) {
    return impl_->start(window, std::move(source), std::move(handler), std::move(config));
}

void AccessibilityHost::stop() {
    impl_->teardown();
}

void AccessibilityHost::mark_dirty() {
    impl_->mark_dirty();
}

bool AccessibilityHost::dirty() const {
    return impl_->is_dirty();
}

bool AccessibilityHost::tick() {
    return impl_->tick();
}

const AccessibilityHostStatus& AccessibilityHost::status() const {
    return impl_->status;
}

bool AccessibilityHost::enabled() const {
    return impl_->status.enabled;
}

ui::a11y::UiaHost* AccessibilityHost::uia_host_for_testing() {
    return impl_->uia.get();
}

ui::a11y::AtspiBridge* AccessibilityHost::atspi_bridge_for_testing() {
    return impl_->bridge.get();
}

ui::a11y::AtspiSession* AccessibilityHost::atspi_session_for_testing() {
    return impl_->session.get();
}

} // namespace tessera::app
