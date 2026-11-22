// SPDX-License-Identifier: MIT

#include "tessera/testing/test_app.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::testing {
namespace {

namespace appfw = tessera::appfw;
namespace ui = tessera::ui;

// One normalized event per dispatched step, on the simulated clock. The step is fixed rather than
// "whatever the clock says", so two runs of the same test stamp the same timestamps.
constexpr double kEventStepSeconds = 1.0 / 60.0;

// `AppHost::paint` needs a sink, and a structural test has nothing to say to one: the frame path
// runs end to end (damage is computed, the plan is walked, commands are emitted) and the assertion
// is about the tree and the counters, never the pixels. Drawing nothing is deliberate - a pixel
// claim belongs in a golden under `tests/golden/`, where a skipped run is visible as a skip.
class DiscardSink final : public ui::paint::PaintSink {
public:
    void fill_rect(const rendering::rhi::Rect& /*rect*/,
                   const rendering::rhi::Color& /*color*/) override {}
    void fill_rounded_rect(const rendering::rhi::Rect& /*rect*/,
                           const rendering::rhi::Color& /*color*/, float /*radius*/) override {}
    void draw_text(const ui::paint::TextDraw& /*run*/) override {}
};

// The one tree walk. A node that is not rendered is skipped together with its subtree, exactly as
// the snapshot walk and the automation host do it: a hidden container does not expose visible
// children either. The two `find` overloads differ only in constness, so they share this rather
// than growing a second definition that could drift.
const ui::Node* find_rendered(const ui::Node& node, std::uint64_t automation_id,
                              std::size_t depth) {
    if (depth > ui::automation::kMaxSnapshotDepth || !node.rendered()) {
        return nullptr;
    }
    if (node.automation_id() == automation_id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const ui::Node* found = find_rendered(*child, automation_id, depth + 1);
            found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

ui::InputEvent pointer_event(ui::InputEventType type, const rendering::rhi::Rect& bounds,
                             double timestamp) {
    ui::InputEvent event;
    event.type = type;
    event.timestamp_s = timestamp;
    // The centre of the target, which is the one point every hit test agrees on even for a rounded
    // or border-heavy box.
    event.x = bounds.x + bounds.width * 0.5f;
    event.y = bounds.y + bounds.height * 0.5f;
    event.pointer_id = 0;
    event.pointer_kind = ui::PointerKind::Mouse;
    event.button = ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

// Largest prefix of `text` that fits `max_bytes` and ends on a UTF-8 sequence boundary. The pump's
// payload is bounded, so a longer string is delivered as several commits, exactly as an IME would.
std::size_t chunk_length(std::string_view text, std::size_t offset, std::size_t max_bytes) {
    std::size_t length = std::min(max_bytes, text.size() - offset);
    while (length > 0 && offset + length < text.size() &&
           (static_cast<unsigned char>(text[offset + length]) & 0xC0U) == 0x80U) {
        --length;
    }
    return length;
}

} // namespace

TestApp::TestApp(Config config)
    : config_(std::move(config)),
      loop_config_(),
      loop_(clock_, loop_config_),
      host_(root_, ui::style::compile_stylesheet(config_.stylesheet), loop_,
            [this] {
                app::HostConfig host_config;
                host_config.width = config_.width;
                host_config.height = config_.height;
                host_config.dpi_scale = config_.dpi_scale;
                return host_config;
            }()),
      pump_(),
      services_(),
      runner_(clock_, completions_, &services_) {
    // The loop is handed the three wakes the app actually runs on: the completion queue (service
    // and task results), the timer queue (AF2's deadlines) and render-on-demand. A test that omits
    // any of them would time out on work the real app completes, and the failure would look like a
    // bug in the app.
    loop_.set_completion_queue(&completions_);
    loop_.set_timer_queue(&timers_);
    // The window driver installs the host as the pump's input filter so Back, Escape and the
    // overlay-dismissal rules see every event before the scene does (issue #302/#445). Without this
    // a `BackRequested` would reach the scene, which has no Back event, and the app's own back
    // handler would never run - the exact bug a test is supposed to be able to catch.
    pump_.set_input_filter(
        [](void* user, ui::Node& /*root*/, const ui::InputEvent& event) {
            return static_cast<app::AppHost*>(user)->handle_overlay_input(event);
        },
        &host_);
}

TestApp::~TestApp() = default;

std::expected<void, appfw::ServiceError> TestApp::prepare() {
    if (prepared_) {
        return {};
    }
    if (const auto frozen = services_.freeze(); !frozen) {
        return frozen;
    }
    if (const auto ready = runner_.prepare(); !ready) {
        return ready;
    }
    prepared_ = true;
    return {};
}

void TestApp::build() {
    if (built_) {
        return;
    }
    built_ = true;
    build_scene();
    // One settled frame so bounds, styles and hit testing are valid before the first interaction: a
    // `tap` on an unmeasured tree hits nothing and would look like a broken locator.
    settle();
}

double TestApp::now() const {
    return clock_.now_seconds();
}

bool TestApp::pump_once() {
    AdvanceConfig defaults = config_.advance;
    last_frames_seen_ = frames_painted_;
    tick_once(defaults);
    return frames_painted_ != last_frames_seen_;
}

void TestApp::advance(double duration, AdvanceConfig config) {
    if (!(duration > 0.0)) {
        return;
    }
    const double tick = config.tick_seconds > 0.0 ? config.tick_seconds : (1.0 / 60.0);
    // A whole number of ticks that covers the requested span, so "advance(0.1) then advance(0.1)"
    // and "advance(0.2)" run the same number of frames and the same animation samples.
    const auto ticks = static_cast<std::size_t>(std::llround(duration / tick));
    pump_ticks(ticks == 0 ? 1 : ticks, config);
}

void TestApp::pump_ticks(std::size_t ticks, AdvanceConfig config) {
    for (std::size_t index = 0; index < ticks; ++index) {
        tick_once(config);
    }
}

void TestApp::tick_once(const AdvanceConfig& config) {
    const double tick = config.tick_seconds > 0.0 ? config.tick_seconds : (1.0 / 60.0);
    clock_.advance(tick);
    ++ticks_run_;

    // The order is the production one: I/O bodies (on the worker half), their completions (on the
    // UI half), then the loop, which fires due timers and drains the completion queue itself.
    // Running the executor after the loop would deliver a service's result one frame late, which a
    // test would see as a mysterious one-tick lag.
    if (config.pump_io) {
        (void)executor_.run(1);
        (void)executor_.run_completions(1);
    }
    (void)completions_.run(loop_config_.completion_budget);
    (void)loop_.tick();
    run_frame(false);
}

void TestApp::run_frame(bool force) {
    const app::HostFrame frame = host_.begin_frame(0.0);
    if (!frame.frame_due && !force) {
        return;
    }
    DiscardSink sink;
    (void)host_.paint(sink, force);
    host_.end_frame();
    ++frames_painted_;
}

void TestApp::settle() {
    // One forced frame: the settle a test wants after it changed the tree or dispatched input,
    // where "was a frame due" is not the question being asked.
    run_frame(true);
}

bool TestApp::tap(std::uint64_t automation_id) {
    ui::Node* target = find(automation_id);
    if (target == nullptr) {
        return false;
    }
    const rendering::rhi::Rect bounds = target->world_bounds();
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height)) {
        return false;
    }
    // The whole gesture goes through the pump's bounded queue, exactly like platform input: hover,
    // implicit capture, focus and click synthesis are the production code paths.
    const double stamp = now();
    const ui::InputEvent move = pointer_event(ui::InputEventType::PointerMove, bounds, stamp);
    const ui::InputEvent down =
        pointer_event(ui::InputEventType::PointerDown, bounds, stamp + kEventStepSeconds);
    const ui::InputEvent up =
        pointer_event(ui::InputEventType::PointerUp, bounds, stamp + 2.0 * kEventStepSeconds);
    if (!pump_.queue().push(move) || !pump_.queue().push(down) || !pump_.queue().push(up)) {
        return false;
    }
    (void)pump_.pump(root_, loop_);
    settle();
    return true;
}

bool TestApp::type_text(std::uint64_t automation_id, std::string_view text) {
    ui::Node* target = find(automation_id);
    if (target == nullptr || !target->focusable()) {
        return false;
    }
    // Programmatic focus goes through the same FocusManager the router drives, then the committed
    // text reaches the focused node as `EditText` through the pump.
    if (!pump_.router().focus_manager().request_focus(target)) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t length = chunk_length(text, offset, ui::kMaxEditTextBytes);
        if (length == 0) {
            return false;
        }
        ui::InputEvent event;
        event.type = ui::InputEventType::EditText;
        event.timestamp_s = now();
        event.text_size = static_cast<std::uint32_t>(length);
        std::copy_n(text.data() + offset, length, event.text.data());
        if (!pump_.queue().push(event)) {
            return false;
        }
        offset += length;
    }
    (void)pump_.pump(root_, loop_);
    settle();
    return true;
}

void TestApp::send(const ui::InputEvent& event) {
    if (!pump_.queue().push(event)) {
        return;
    }
    (void)pump_.pump(root_, loop_);
    settle();
}

void TestApp::dispatch_os_events() {
    // The app has no window driver, so it observes the injector's events through the one thing a
    // headless app actually owns: the geometry/media context every display, DPI and safe-area event
    // carries in its metrics. Lifecycle and style events belong to a service layer and a window
    // driver that a bare `TestApp` does not install, so they are drained and dropped rather than
    // silently left in the ring for the next call to mis-read.
    hal::OsEvent event;
    bool geometry_moved = false;
    while (os_events_.pop(event)) {
        switch (event.type) {
        case hal::OsEventType::WindowResized:
        case hal::OsEventType::DpiChanged:
        case hal::OsEventType::ScaleChanged:
        case hal::OsEventType::OrientationChanged:
        case hal::OsEventType::SafeAreaChanged:
        case hal::OsEventType::WorkAreaChanged:
            geometry_moved = true;
            break;
        default:
            break;
        }
    }
    if (geometry_moved) {
        host_.set_window_metrics(os_events_.metrics());
        settle();
    }
}

void TestApp::press(hal::SystemButton button) {
    if (os_events_.press(button) != hal::InjectStatus::Queued) {
        return;
    }
    dispatch_os_events();
    (void)os_events_.feed_input(pump_.queue());
    (void)pump_.pump(root_, loop_);
    settle();
}

ui::Node* TestApp::find(std::uint64_t automation_id) {
    // The const walk is the definition; the non-const result re-derives the mutable pointer from
    // the same path, so `tap` and `text_of` cannot locate two different nodes.
    const ui::Node* found = find_rendered(root_, automation_id, 0);
    return found == nullptr ? nullptr : const_cast<ui::Node*>(found);
}

const ui::Node* TestApp::find(std::uint64_t automation_id) const {
    return find_rendered(root_, automation_id, 0);
}

std::optional<std::string> TestApp::text_of(std::uint64_t automation_id) const {
    const ui::Node* node = find(automation_id);
    if (node == nullptr) {
        return std::nullopt;
    }
    const std::optional<ui::PaintData>& data = node->paint_data();
    if (!data.has_value()) {
        return std::nullopt;
    }
    return data->text;
}

std::optional<ui::automation::UiSnapshot> TestApp::snapshot(bool include_hidden) const {
    ui::automation::SceneSnapshotOptions options;
    options.snapshot.include_hidden = include_hidden;
    const auto built = ui::automation::snapshot_scene(root_, options);
    if (!built) {
        return std::nullopt;
    }
    return *built;
}

std::size_t TestApp::count_nodes(std::uint64_t automation_id) const {
    std::size_t count = 0;
    count_in(root_, automation_id, count);
    return count;
}

void TestApp::count_in(const ui::Node& node, std::uint64_t automation_id,
                       std::size_t& count) const {
    if (node.automation_id() == automation_id) {
        ++count;
    }
    for (const auto& child : node.children()) {
        count_in(*child, automation_id, count);
    }
}

bool TestApp::expect_text(std::uint64_t automation_id, std::string_view expected,
                          std::string_view what) const {
    const std::optional<std::string> actual = text_of(automation_id);
    if (!actual.has_value()) {
        return expect(false, std::string(what) + ": node " + std::to_string(automation_id) +
                                 " has no text payload");
    }
    if (*actual == expected) {
        return true;
    }
    return expect(false, std::string(what) + ": node " + std::to_string(automation_id) +
                             " text is \"" + *actual + "\", expected \"" + std::string(expected) +
                             "\"");
}

bool TestApp::expect_exists(std::uint64_t automation_id, std::string_view what) const {
    if (find(automation_id) != nullptr) {
        return true;
    }
    return expect(false,
                  std::string(what) + ": node " + std::to_string(automation_id) + " is absent");
}

bool TestApp::expect_absent(std::uint64_t automation_id, std::string_view what) const {
    if (find(automation_id) == nullptr) {
        return true;
    }
    return expect(false, std::string(what) + ": node " + std::to_string(automation_id) +
                             " is present but should be absent");
}

} // namespace tessera::testing
