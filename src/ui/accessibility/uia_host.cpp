// SPDX-License-Identifier: MIT
//
// Window/host wiring for the Windows UI Automation bridge: the root-provider handshake and the
// WM_GETOBJECT answer. The bridge thread owns the root object; this file never calls UIA on the
// window thread and never waits for the bridge.

#include "tessera/ui/accessibility/uia_host.hpp"

#include <utility>

namespace tessera::ui::a11y {

UiaHost::UiaHost(UiaHostConfig config, std::unique_ptr<UiaApartment> apartment,
                 std::unique_ptr<UiaPlatform> platform)
    : config_(config),
      platform_(platform != nullptr ? std::move(platform) : create_platform_uia()),
      bridge_(config_.bridge, std::move(apartment)) {}

UiaHost::~UiaHost() {
    stop();
}

std::expected<void, UiaError> UiaHost::start() {
    if (platform_ == nullptr) {
        // A build without a UIA platform seam: accessibility stays off with a diagnostic, the app
        // still runs (spec section 7).
        return std::unexpected(UiaError::UnsupportedPlatform);
    }
    // Hooks capture `this`; they run on the bridge thread and the bridge is always stopped (and
    // joined) before this host is destroyed.
    UiaBridgeHooks hooks;
    hooks.on_start = [this](UiaProvider& provider) { on_bridge_start(provider); };
    hooks.on_stop = [this](UiaProvider& provider) { on_bridge_stop(provider); };
    bridge_.set_hooks(std::move(hooks));
    return bridge_.start();
}

void UiaHost::stop() {
    // `stop` joins the bridge worker, so no hook can run after it returns. The root pointer is
    // cleared before the pointer is released (the stop hook does that on the bridge thread).
    bridge_.stop();
    root_.store(nullptr, std::memory_order_release);
}

bool UiaHost::ready() const {
    return root_.load(std::memory_order_acquire) != nullptr;
}

bool UiaHost::answer_get_object(std::intptr_t object_id, void*& raw_provider) const {
    // Called from the window procedure: one atomic load, no lock, no COM. A not-ready bridge
    // returns false so the window keeps its default accessibility provider instead of failing.
    if (object_id != kUiaRootObjectId) {
        return false;
    }
    void* root = root_.load(std::memory_order_acquire);
    if (root == nullptr) {
        return false;
    }
    raw_provider = root;
    return true;
}

UiaBridge& UiaHost::bridge() {
    return bridge_;
}

const UiaBridge& UiaHost::bridge() const {
    return bridge_;
}

UiaError UiaHost::error() const {
    return bridge_.error();
}

void UiaHost::on_bridge_start(UiaProvider&) {
    // Bridge thread, after `CoInitializeEx`. Creating the provider here is what makes the object
    // bridge-thread-owned: its COM methods marshal to this same thread and run inline.
    void* root = platform_->create_root(bridge_);
    root_.store(root, std::memory_order_release);
}

void UiaHost::on_bridge_stop(UiaProvider&) {
    // Stop answering before releasing: a WM_GETOBJECT racing shutdown must see "not ready" rather
    // than a dangling provider.
    root_.store(nullptr, std::memory_order_release);
    platform_->destroy_root();
}

} // namespace tessera::ui::a11y
