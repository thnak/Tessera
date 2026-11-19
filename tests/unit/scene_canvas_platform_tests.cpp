// SPDX-License-Identifier: MIT
//
// `platform.event` (issue #446, designer-protocol-v1 section 5.1): the hardware buttons and
// environment events of a device profile, delivered to the running canvas session through the
// synthetic injector, and the same script through the scripted run the parity gate compares.
//
// The cases follow the acceptance of studio-device-profiles-v1.md section 4: an unhandled Back
// closes the app and a relaunch is a cold start; Home then Resume is a hot resume that keeps the
// count; process death after a Suspend relaunches with the saved cells; Power turns the screen off
// and back on; and an orientation or fold event re-lays the running tree out in place.

#include "scene_canvas_test_support.hpp"
#include "scene_interaction.hpp"
#include "scene_platform.hpp"

#include <string>
#include <string_view>

namespace {

using namespace canvas_test;

cli::PlatformEvent make_event(cli::PlatformEventKind kind, std::string_view value = {},
                              std::int64_t timestamp_ns = -1) {
    cli::PlatformEvent event;
    event.kind = kind;
    event.value = std::string(value);
    event.timestamp_ns = timestamp_ns;
    return event;
}

// A started session whose counter has been clicked `clicks` times.
void start_counter(cli::CanvasSceneSession& session, int clicks) {
    const std::string error = session.start(source_for(kCounter, "ui/Counter.tsx"));
    expect(error.empty(), "the counter starts: " + error);
    const ui::Node* button = find_kind(*session.root(), "Button");
    expect(button != nullptr, "the counter has a button");
    if (button == nullptr) {
        return;
    }
    const Point at = centre_of(*button);
    for (int index = 0; index < clicks; ++index) {
        static_cast<void>(session.pointer(cli::PreviewInput::Kind::PointerMove, at.x, at.y));
        static_cast<void>(session.pointer(cli::PreviewInput::Kind::PointerDown, at.x, at.y));
        static_cast<void>(session.pointer(cli::PreviewInput::Kind::PointerUp, at.x, at.y));
    }
}

void test_vocabulary() {
    cli::PlatformEventKind kind = cli::PlatformEventKind::Back;
    for (const std::string_view name :
         {"back",         "home",      "recents",      "volumeUp",  "volumeDown", "power",
          "suspend",      "resume",    "activate",     "screenOff", "screenOn",   "theme",
          "locale",       "textScale", "orientation",  "ime",       "fold",       "memoryPressure",
          "batterySaver", "thermal",   "processDeath", "relaunch"}) {
        expect(cli::platform_kind_from(name, kind), "known event name: " + std::string(name));
        expect(cli::platform_kind_name(kind) == name, "the name round-trips: " + std::string(name));
    }
    expect(!cli::platform_kind_from("kill", kind), "an unknown name is refused, not guessed");
    expect(!cli::platform_kind_from("Back", kind), "names are case-sensitive");
}

void test_back_closes_the_app_and_relaunch_is_cold() {
    cli::CanvasSceneSession session;
    start_counter(session, 2);
    expect(counter_value(session) == 2, "the counter was clicked twice");

    const cli::PlatformReply back =
        session.platform_event(make_event(cli::PlatformEventKind::Back));
    expect(back.ok, "Back is accepted");
    expect(back.app_closed && back.handled, "an unhandled Back closes the app");
    expect(back.dispatch == "unhandled", "the preview has no overlay and no route: unhandled");
    expect(back.state == cli::AppLife::Closed, "the app is closed");

    const cli::PlatformReply volume =
        session.platform_event(make_event(cli::PlatformEventKind::VolumeUp));
    expect(volume.ok && !volume.handled && volume.state == cli::AppLife::Closed,
           "a closed app receives no key");

    const cli::PlatformReply relaunch =
        session.platform_event(make_event(cli::PlatformEventKind::Relaunch));
    expect(relaunch.ok && relaunch.relaunch && !relaunch.restore,
           "relaunch after an unhandled Back restores nothing");
    expect(!session.started(), "the relaunch ended the evaluation");
    expect(session.app_life() == cli::AppLife::Running, "the new process is running");

    const std::string error = session.start(source_for(kCounter, "ui/Counter.tsx"));
    expect(error.empty(), "the cold start evaluates: " + error);
    expect(counter_value(session) == 0, "a cold start resets the count");
}

void test_home_then_resume_is_a_hot_resume() {
    cli::CanvasSceneSession session;
    start_counter(session, 3);

    const cli::PlatformReply home =
        session.platform_event(make_event(cli::PlatformEventKind::Home));
    expect(home.ok && home.handled, "Home is handled");
    expect(home.state == cli::AppLife::Background, "Home puts the app in the background");
    expect(home.saved_state, "the Suspend captured the saved-state record");
    expect(!home.app_closed, "Home does not close the app");
    expect(session.started() && counter_value(session) == 3, "the tree and the count are kept");

    const cli::PlatformReply back_while_hidden =
        session.platform_event(make_event(cli::PlatformEventKind::Back));
    expect(back_while_hidden.ok && !back_while_hidden.handled &&
               back_while_hidden.state == cli::AppLife::Background,
           "a backgrounded app receives no Back");

    const cli::PlatformReply resume =
        session.platform_event(make_event(cli::PlatformEventKind::Resume));
    expect(resume.ok && resume.resumed && resume.state == cli::AppLife::Running,
           "Resume brings the app back");
    expect(session.started() && counter_value(session) == 3, "a hot resume keeps the count");
}

void test_process_death_relaunches_from_the_saved_state() {
    cli::CanvasSceneSession session;
    start_counter(session, 4);

    static_cast<void>(session.platform_event(make_event(cli::PlatformEventKind::Home)));
    const cli::PlatformReply death =
        session.platform_event(make_event(cli::PlatformEventKind::ProcessDeath));
    expect(death.ok && death.app_closed && death.saved_state,
           "the process died with a saved record");
    expect(death.state == cli::AppLife::Closed, "the app is closed");

    const cli::PlatformReply relaunch =
        session.platform_event(make_event(cli::PlatformEventKind::Relaunch));
    expect(relaunch.relaunch && relaunch.restore, "the relaunch restores the saved state");
    expect(!session.started(), "the evaluation is gone until the page applies again");

    const std::string error = session.start(source_for(kCounter, "ui/Counter.tsx"));
    expect(error.empty(), "the relaunch evaluates: " + error);
    expect(counter_value(session) == 4, "the saved cells come back");

    // A second restart with no Suspend in between has nothing saved.
    static_cast<void>(session.platform_event(make_event(cli::PlatformEventKind::ProcessDeath)));
    const cli::PlatformReply cold =
        session.platform_event(make_event(cli::PlatformEventKind::Relaunch));
    expect(cold.relaunch && !cold.restore, "death with no Suspend relaunches cold");
    expect(session.start(source_for(kCounter, "ui/Counter.tsx")).empty(), "it evaluates");
    expect(counter_value(session) == 0, "a cold relaunch starts at the initializer");

    const cli::PlatformReply running =
        session.platform_event(make_event(cli::PlatformEventKind::Relaunch));
    expect(!running.ok && running.error.find("TESSERA_PLATFORM_NOT_CLOSED") == 0,
           "relaunching a running app is refused with its code");
}

void test_power_and_recents() {
    cli::CanvasSceneSession session;
    start_counter(session, 1);

    const cli::PlatformReply off =
        session.platform_event(make_event(cli::PlatformEventKind::Power));
    expect(off.handled && off.state == cli::AppLife::ScreenOff, "Power turns the screen off");
    const cli::PlatformReply key =
        session.platform_event(make_event(cli::PlatformEventKind::VolumeDown));
    expect(key.ok && !key.handled, "a key with the screen off reaches nobody");
    const cli::PlatformReply wake =
        session.platform_event(make_event(cli::PlatformEventKind::Power));
    expect(wake.resumed && wake.state == cli::AppLife::Running, "Power again wakes the app");
    expect(counter_value(session) == 1, "the count is kept across a screen cycle");

    const cli::PlatformReply recents =
        session.platform_event(make_event(cli::PlatformEventKind::Recents));
    expect(recents.handled && recents.state == cli::AppLife::Inactive,
           "Recents leaves the app running but inactive");
    const cli::PlatformReply back =
        session.platform_event(make_event(cli::PlatformEventKind::Activate));
    expect(back.resumed && back.state == cli::AppLife::Running, "activating returns to running");
}

void test_volume_reaches_the_focused_node() {
    cli::CanvasSceneSession session;
    start_counter(session, 0);
    const cli::PlatformReply none =
        session.platform_event(make_event(cli::PlatformEventKind::VolumeUp));
    expect(none.ok && !none.handled, "with nothing focused the key is dropped");

    const ui::Node* button = find_kind(*session.root(), "Button");
    const Point at = centre_of(*button);
    static_cast<void>(session.pointer(cli::PreviewInput::Kind::PointerDown, at.x, at.y));
    const cli::CanvasPointerResult up =
        session.pointer(cli::PreviewInput::Kind::PointerUp, at.x, at.y);
    const cli::PlatformReply focused =
        session.platform_event(make_event(cli::PlatformEventKind::VolumeUp));
    expect(focused.ok && focused.handled == (up.focused_id != 0),
           "the key is dispatched exactly when a node holds focus");
}

void test_environment_events() {
    cli::CanvasSceneSession session;
    start_counter(session, 1);
    expect(session.metrics().logical_width == 320 && session.metrics().logical_height == 240,
           "the session starts landscape-shaped at 320x240");

    const cli::PlatformReply portrait =
        session.platform_event(make_event(cli::PlatformEventKind::Orientation, "portrait"));
    expect(portrait.ok && portrait.handled && portrait.layout_changed, "orientation re-lays out");
    expect(session.metrics().logical_width == 240 && session.metrics().logical_height == 320,
           "the extent swapped in place");
    expect(session.needs_frame(), "a frame is owed after the layout pass");
    expect(counter_value(session) == 1, "orientation keeps the state cells");

    const cli::PlatformReply again =
        session.platform_event(make_event(cli::PlatformEventKind::Orientation, "portrait"));
    expect(again.ok && !again.layout_changed, "the same orientation moves nothing");

    const cli::PlatformReply bad =
        session.platform_event(make_event(cli::PlatformEventKind::Orientation, "sideways"));
    expect(!bad.ok && bad.error.find("TESSERA_PLATFORM_BAD_VALUE") == 0, "a bad value has a code");

    const cli::PlatformReply fold =
        session.platform_event(make_event(cli::PlatformEventKind::Fold, "book"));
    expect(fold.ok && fold.handled && fold.layout_changed, "a fold posture is applied in place");
    const cli::PlatformReply unfold =
        session.platform_event(make_event(cli::PlatformEventKind::Fold, "none"));
    expect(unfold.ok && unfold.layout_changed, "and removed again");

    for (const cli::PlatformEventKind kind :
         {cli::PlatformEventKind::Theme, cli::PlatformEventKind::Locale,
          cli::PlatformEventKind::TextScale, cli::PlatformEventKind::Ime,
          cli::PlatformEventKind::MemoryPressure, cli::PlatformEventKind::BatterySaver,
          cli::PlatformEventKind::Thermal}) {
        const cli::PlatformReply reply = session.platform_event(make_event(kind, "dark"));
        expect(reply.ok && !reply.handled && reply.state == cli::AppLife::Running,
               "an event the tree does not read is delivered and reported unhandled: " +
                   std::string(cli::platform_kind_name(kind)));
    }
}

void test_sequence_and_clock() {
    cli::CanvasSceneSession session;
    start_counter(session, 0);
    const cli::PlatformReply first =
        session.platform_event(make_event(cli::PlatformEventKind::Theme, "dark", 1'000'000));
    const cli::PlatformReply second =
        session.platform_event(make_event(cli::PlatformEventKind::Theme, "light", 5'000'000));
    const cli::PlatformReply late =
        session.platform_event(make_event(cli::PlatformEventKind::Theme, "dark", 2'000'000));
    expect(first.sequence == 1 && second.sequence == 2 && late.sequence == 3,
           "the sequence counts every accepted event");
    expect(first.timestamp_ns == 1'000'000 && second.timestamp_ns == 5'000'000,
           "the simulated timestamp is the one the host sent");
    expect(late.timestamp_ns == 5'000'000,
           "a timestamp behind the clock is clamped, never rewound");
}

void test_no_session() {
    cli::CanvasSceneSession session;
    const cli::PlatformReply reply =
        session.platform_event(make_event(cli::PlatformEventKind::Back));
    expect(!reply.ok && reply.error.find("TESSERA_PLATFORM_NO_SESSION") == 0,
           "an event before the first apply names the missing session");
}

// The scripted run and the canvas session run one driver: the same events produce the same replies
// and the same final state cells (the parity gate compares the native and Wasm documents of this
// run; this proves the two entry points agree with each other).
void test_script_and_canvas_agree() {
    const std::string script = "platform home @1000\nplatform resume @2000\nplatform back @3000\n";
    cli::ScenePreviewOptions options;
    options.file = "ui/Counter.tsx";
    const cli::InputRunResult run = cli::run_input_script(
        kCounter, options, "Counter", "platform home\nplatform resume\nplatform back\n");
    expect(run.built, "the scripted run builds: " + run.error);
    if (!run.built) {
        return;
    }
    expect(run.platforms.size() == 3 && run.kinds[0] == "platform", "three platform steps ran");
    expect(run.platforms[0].state == cli::AppLife::Background, "scripted Home backgrounds");
    expect(run.platforms[1].resumed, "scripted Resume resumes");
    expect(run.platforms[2].app_closed, "scripted Back closes");

    cli::CanvasSceneSession session;
    start_counter(session, 0);
    const cli::PlatformEventKind sequence[] = {
        cli::PlatformEventKind::Home, cli::PlatformEventKind::Resume, cli::PlatformEventKind::Back};
    for (std::size_t index = 0; index < 3; ++index) {
        const cli::PlatformReply reply = session.platform_event(make_event(sequence[index]));
        expect(cli::platform_reply_json(reply) == cli::platform_reply_json(run.platforms[index]),
               "step " + std::to_string(index) + " answers the same in both entry points");
    }
    expect(session.state().size() == run.states.back().size(),
           "the two runs end with the same cells");

    const std::string document = cli::scene_input_document(kCounter, options, "Counter", script);
    expect(document.find("\"kind\":\"platform\"") != std::string::npos &&
               document.find("\"timestampNs\":3000") != std::string::npos,
           "the scripted document carries the platform replies: " + document);
}

void test_script_errors() {
    std::vector<cli::PreviewInputStep> steps;
    std::string error;
    expect(!cli::parse_input_script("platform kill\n", steps, error) &&
               error.find("not a platform event type") != std::string::npos,
           "an unknown event type in a script is an error, not a skipped line");
    expect(!cli::parse_input_script("platform theme dark light\n", steps, error),
           "a platform step takes one value");
    expect(!cli::parse_input_script("platform back @soon\n", steps, error),
           "the timestamp is a number");
    expect(cli::parse_input_script("platform orientation landscape @42\nclick 7\n", steps, error) &&
               steps.size() == 2 && steps[0].platform.value == "landscape" &&
               steps[0].platform.timestamp_ns == 42 && steps[1].kind == "click",
           "platform and click steps mix");
}

} // namespace

int main() {
    test_vocabulary();
    test_back_closes_the_app_and_relaunch_is_cold();
    test_home_then_resume_is_a_hot_resume();
    test_process_death_relaunches_from_the_saved_state();
    test_power_and_recents();
    test_volume_reaches_the_focused_node();
    test_environment_events();
    test_sequence_and_clock();
    test_no_session();
    test_script_and_canvas_agree();
    test_script_errors();
    if (failures() != 0) {
        std::cerr << "scene_canvas_platform: " << failures() << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_canvas_platform: ok\n";
    return 0;
}
