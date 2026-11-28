// SPDX-License-Identifier: MIT
//
// The canvas preview animates (issue #603). Before this the session ran no animation runtime and
// asked the page for no second frame, so `:hover` changed style instantly, `@keyframes` never
// played and a transition could not be seen. These tests drive the session the way the page does -
// an `animate(now)` with the frame timestamp, then a draw when a frame is owed - with explicit
// timestamps instead of a wall clock, so every value asserted here is a function of the sequence.
//
//  1. **`@keyframes` play**: the animated opacity moves with the timestamps and ends at the fill.
//  2. **A `:hover` transition runs** from the resting value to the hover value, then the session
//     goes idle: the keep-alive is false and no further frame is owed.
//  3. **Idle is zero frames**: a sheet with no motion owes nothing after its first frame, however
//     many timestamps the page offers.
//  4. **Reduced motion** settles the animation at once, and survives a re-evaluation.
//  5. **State survives**: a click (a re-evaluation of the tree) and a viewport change in the middle
//     of an animation neither stop it nor restart it.

#include "scene_canvas_motion.hpp"
#include "scene_canvas_test_support.hpp"

#include <cmath>

namespace {

using namespace canvas_test;

constexpr double kFrame = 1.0 / 60.0;

constexpr std::string_view kPulse = R"tsx(import "./Preview.css";

export function Pulse(): Node {
  const [count, setCount] = useState<int>(0);
  return (
    <View>
      <View className="pulse"><Text>Pulse</Text></View>
      <Button onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
)tsx";

constexpr std::string_view kPulseCss = R"(.pulse { opacity: 1; animation: fade 1s linear forwards; }
@keyframes fade { from { opacity: 1; } to { opacity: 0; } }
)";

constexpr std::string_view kHover = R"tsx(import "./Preview.css";

export function Hover(): Node {
  return (
    <View>
      <Button className="btn">Go</Button>
    </View>
  );
}
)tsx";

constexpr std::string_view kHoverCss =
    R"(.btn { opacity: 1; transition: opacity 200ms linear; }
.btn:hover { opacity: 0.2; }
)";

constexpr std::string_view kStaticCss = R"(.pulse { opacity: 0.5; }
)";

cli::CanvasSessionSource with_css(std::string_view text, std::string_view file,
                                  std::string_view css) {
    cli::CanvasSessionSource source = source_for(text, file);
    source.stylesheet = css;
    source.stylesheet_file = "ui/Preview.css";
    return source;
}

// The opacity the paint pass would use for `node` right now: the computed style through the real
// cascade with the animation driver's view laid over it, which is exactly the paint pass's read.
float opacity_of(const cli::CanvasSceneSession& session, const ui::Node& node) {
    ui::style::SceneStyleTree tree(*session.root());
    ui::style::ComputedStyle style = tree.compute(session.sheet(), node);
    if (session.motion() != nullptr) {
        session.motion()->view().overlay_style(node.id(), style);
    }
    const std::string* value = style.find("opacity");
    return value != nullptr ? std::stof(*value) : 1.0f;
}

bool near(float lhs, float rhs, float tolerance = 0.02f) {
    return std::abs(lhs - rhs) <= tolerance;
}

// One page frame: tick the clock, then draw if the session owes one. Returns whether it drew.
bool frame(cli::CanvasSceneSession& session, CanvasHarness& canvas, double now) {
    (void)session.animate(now);
    return canvas.draw(session);
}

// The page's frame loop for `seconds` of display time at 60 Hz after `now`: one frame per refresh
// whenever the session owes one. Advances `now` and returns how many frames it drew. A gap is one
// display refresh, which keeps every step under the driver's hidden-tab clamp.
int play(cli::CanvasSceneSession& session, CanvasHarness& canvas, double& now, double seconds) {
    int drawn = 0;
    const int steps = static_cast<int>(std::lround(seconds / kFrame));
    for (int step = 0; step < steps; ++step) {
        now += kFrame;
        if (frame(session, canvas, now)) {
            ++drawn;
        }
    }
    return drawn;
}

void test_keyframes_play_and_stop() {
    cli::CanvasSceneSession session;
    expect(session.start(with_css(kPulse, "ui/Pulse.tsx", kPulseCss)).empty(), "the source starts");
    const ui::Node* pulse = find_class(*session.root(), "pulse");
    expect(pulse != nullptr, "the animated node exists");
    CanvasHarness canvas;
    if (pulse == nullptr || !canvas.ready()) {
        return;
    }
    expect(session.needs_frame(), "a fresh evaluation owes its first frame");

    // The page's first frame anchors the clock: elapsed 0, so the node is still fully opaque, and
    // the animation the sheet declares is running.
    double now = 10.0;
    expect(frame(session, canvas, now), "the first frame is drawn");
    expect(session.animating(), "a stylesheet animation runs from the first frame");
    expect(near(opacity_of(session, *pulse), 1.0f), "the animation starts at its from value");

    // Half a second of display time later the linear 1s fade is halfway, and every one of those
    // refreshes drew a frame: the keep-alive.
    const int drawn = play(session, canvas, now, 0.5);
    expect(drawn == 30, "an animation draws a frame per refresh, got " + std::to_string(drawn));
    const float middle = opacity_of(session, *pulse);
    expect(near(middle, 0.5f, 0.03f),
           "half way through a linear 1s fade, got " + std::to_string(middle));
    expect(session.animating(), "the animation is still running at 0.5s");

    // Past the end: the fill holds the last value and the session stops asking for frames.
    (void)play(session, canvas, now, 0.6);
    expect(near(opacity_of(session, *pulse), 0.0f), "the forwards fill holds the to value");
    expect(!session.animating(), "a finished animation stops the keep-alive");
    expect(!session.needs_frame(), "and owes no frame once its last one is painted");

    // Idle: the page offers timestamps and nothing is owed.
    const int before = canvas.frames;
    expect(play(session, canvas, now, 1.0) == 0, "an idle session draws nothing");
    expect(canvas.frames == before, "an idle session presents zero frames");
    expect(near(opacity_of(session, *pulse), 0.0f), "the fill still holds while idle");
}

void test_hover_transition_runs_then_idles() {
    cli::CanvasSceneSession session;
    expect(session.start(with_css(kHover, "ui/Hover.tsx", kHoverCss)).empty(), "the source starts");
    const ui::Node* button = find_class(*session.root(), "btn");
    CanvasHarness canvas;
    if (button == nullptr || !canvas.ready()) {
        return;
    }
    double now = 1.0;
    expect(frame(session, canvas, now), "the first frame is drawn");
    expect(!session.animating(), "nothing animates at rest");
    expect(near(opacity_of(session, *button), 1.0f), "the button rests at opacity 1");
    expect(play(session, canvas, now, 0.5) == 0, "an idle scene draws nothing");

    // The hover owes a frame; the tick that precedes it starts the transition from the resting
    // value instead of snapping to the hover one.
    const Point centre = centre_of(*button);
    const cli::CanvasPointerResult hover =
        session.pointer(cli::PreviewInput::Kind::PointerMove, centre.x, centre.y);
    expect(hover.changed, "the hover owes a frame");
    now += kFrame;
    expect(frame(session, canvas, now), "the hover frame is drawn");
    expect(session.animating(), "the hover started a transition");
    expect(near(opacity_of(session, *button), 1.0f, 0.05f),
           "the transition starts at the resting value, not at the hover one");

    (void)play(session, canvas, now, 0.1);
    const float middle = opacity_of(session, *button);
    expect(middle < 0.9f && middle > 0.3f,
           "mid transition the opacity is between the two values, got " + std::to_string(middle));

    expect(play(session, canvas, now, 0.3) > 0, "the rest of the transition is drawn");
    expect(!session.animating(), "the transition ended");
    expect(near(opacity_of(session, *button), 0.2f), "the button rests at the hover opacity");

    const int before = canvas.frames;
    expect(play(session, canvas, now, 1.0) == 0,
           "an idle frame after the transition draws nothing");
    expect(canvas.frames == before, "zero frames once the transition ended");

    // Leaving transitions back, the same way.
    (void)session.pointer(cli::PreviewInput::Kind::PointerMove, kOutside.x, kOutside.y);
    now += kFrame;
    expect(frame(session, canvas, now), "the leave frame is drawn");
    expect(session.animating(), "the leave started a transition");
    (void)play(session, canvas, now, 0.1);
    const float back = opacity_of(session, *button);
    expect(back > 0.3f && back < 0.9f,
           "mid leave the opacity is between the values, got " + std::to_string(back));
    (void)play(session, canvas, now, 0.3);
    expect(near(opacity_of(session, *button), 1.0f), "the button is back at opacity 1");
    expect(!session.animating(), "the leave transition ended");
}

void test_static_sheet_is_idle() {
    cli::CanvasSceneSession session;
    expect(session.start(with_css(kPulse, "ui/Pulse.tsx", kStaticCss)).empty(),
           "the source starts");
    CanvasHarness canvas;
    if (!canvas.ready()) {
        return;
    }
    double now = 1.0;
    expect(frame(session, canvas, now), "the Run's frame is drawn");
    expect(!session.animating(), "a sheet without motion never animates");
    expect(play(session, canvas, now, 1.0) == 0, "a static scene draws nothing");
    expect(canvas.frames == 1, "a static scene presented exactly its first frame");
}

void test_reduced_motion_settles_at_once() {
    cli::CanvasSceneSession session;
    // Set before the evaluation, as the page does with its first tick: it applies to the driver the
    // evaluation makes, so the animation the sheet declares is clamped from the moment it starts.
    session.set_reduced_motion(true);
    expect(session.start(with_css(kPulse, "ui/Pulse.tsx", kPulseCss)).empty(), "the source starts");
    const ui::Node* pulse = find_class(*session.root(), "pulse");
    CanvasHarness canvas;
    if (pulse == nullptr || !canvas.ready()) {
        return;
    }
    double now = 1.0;
    expect(frame(session, canvas, now), "the first frame is drawn");
    (void)play(session, canvas, now, 0.3);
    expect(!session.animating(), "reduced motion ends a 1s animation within a third of a second");
    expect(near(opacity_of(session, *pulse), 0.0f), "it ends at its fill value, not part way");
}

void test_state_survives_animation_frames() {
    cli::CanvasSceneSession session;
    expect(session.start(with_css(kPulse, "ui/Pulse.tsx", kPulseCss)).empty(), "the source starts");
    CanvasHarness canvas;
    if (!canvas.ready()) {
        return;
    }
    double now = 5.0;
    expect(frame(session, canvas, now), "the first frame is drawn");
    (void)play(session, canvas, now, 0.4);
    const ui::Node* before = find_class(*session.root(), "pulse");
    const float at_start = before != nullptr ? opacity_of(session, *before) : -1.0f;
    expect(near(at_start, 0.6f, 0.03f), "0.4s into the fade, got " + std::to_string(at_start));

    // A click re-evaluates the tree (the handler changed state). The animation neither stops nor
    // restarts: the node keeps the progress it had.
    const ui::Node* button = find_kind(*session.root(), "Button");
    expect(button != nullptr, "the source has a button");
    if (button == nullptr) {
        return;
    }
    const Point at = centre_of(*button);
    (void)session.pointer(cli::PreviewInput::Kind::PointerDown, at.x, at.y);
    (void)session.pointer(cli::PreviewInput::Kind::PointerUp, at.x, at.y);
    expect(counter_value(session) == 1, "the click ran its handler");
    (void)play(session, canvas, now, 0.1);
    expect(session.animating(), "the animation survived the re-evaluation");
    const ui::Node* after = find_class(*session.root(), "pulse");
    const float mid = after != nullptr ? opacity_of(session, *after) : -1.0f;
    expect(near(mid, 0.5f, 0.03f),
           "the fade carried on from 0.4s to 0.5s, got " + std::to_string(mid));

    // A viewport change re-lays the tree out in place: same animation, same state.
    expect(session.set_viewport(300.0f, 200.0f, 1.0f), "the viewport changed");
    (void)play(session, canvas, now, 0.1);
    expect(session.animating(), "the animation survived the viewport change");
    expect(counter_value(session) == 1, "the state survived the viewport change");
    const ui::Node* resized = find_class(*session.root(), "pulse");
    const float late = resized != nullptr ? opacity_of(session, *resized) : -1.0f;
    expect(near(late, 0.4f, 0.03f), "the fade carried on to 0.6s, got " + std::to_string(late));
}

void test_tick_report() {
    cli::CanvasMotionTick tick;
    tick.running = true;
    tick.dt = 1.0 / 60.0;
    tick.samples = 2;
    const std::string json = cli::canvas_tick_report_json(tick, false);
    expect(json == "{\"tickVersion\":1,\"running\":true,\"dtMs\":16.667,\"samples\":2,"
                   "\"reducedMotion\":false}",
           "the tick report is one stable line, got " + json);

    // Without a session the answer is the idle one: nothing runs and nothing is owed.
    cli::CanvasSceneSession none;
    const cli::CanvasMotionTick idle = none.animate(1.0);
    expect(!idle.running && !none.animating() && !none.needs_frame(),
           "a session with no tree is idle");
}

} // namespace

int main() {
    test_keyframes_play_and_stop();
    test_hover_transition_runs_then_idles();
    test_static_sheet_is_idle();
    test_reduced_motion_settles_at_once();
    test_state_survives_animation_frames();
    test_tick_report();
    if (failures() != 0) {
        std::cerr << "scene_canvas_motion: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_motion: ok\n";
    return 0;
}
