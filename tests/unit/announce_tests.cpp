// SPDX-License-Identifier: MIT
//
// `announce` (ADR-064 item 7, issue #629): announce a message to assistive technology.
//
// The engine owns the mechanism (`accessibility::AnnouncementQueue`, drained by the accessibility
// bridge); this behavior owns the hook's half: the message option, the channel, and the rule that a
// repeated message is not a new announcement.

#include "tessera/ui/accessibility/announcement_queue.hpp"
#include "tessera/ui/behavior/announce.hpp"

#include <array>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::accessibility::Announcement;
using tessera::ui::accessibility::AnnouncementQueue;
using tessera::ui::accessibility::LiveChannel;
using tessera::ui::behavior::Announce;
using tessera::ui::behavior::ArgsView;
using tessera::ui::behavior::ArgValue;
using tessera::ui::behavior::OutputValue;
using tessera::ui::behavior::Services;
using tessera::ui::behavior::Targets;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "announce: " << message << "\n";
        ++failures;
    }
}

ArgValue string_arg(std::string_view value) {
    ArgValue arg;
    arg.kind = ArgValue::Kind::String;
    arg.text = value;
    return arg;
}

// The manifest's option order: `message`, `channel`.
std::array<ArgValue, 2> options(std::string_view message, std::string_view channel = "polite") {
    return {string_arg(message), string_arg(channel)};
}

void test_attach_queues_the_message() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("Saved");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    expect(queue.size() == 1, "the message is queued");
    const auto outputs = behavior.outputs();
    expect(outputs.size() == 1 && outputs[0].kind == OutputValue::Kind::Bool && outputs[0].boolean,
           "the `announced` output reports the message was accepted");
    Announcement out;
    expect(queue.take_next(out), "the message drains");
    expect(out.text == "Saved", "the text is carried");
    expect(out.channel == LiveChannel::Polite, "the default channel is polite");
}

void test_assertive_channel() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("Error", "assertive");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    Announcement out;
    expect(queue.take_next(out), "the message drains");
    expect(out.channel == LiveChannel::Assertive, "the assertive channel is carried");
}

void test_off_channel_queues_nothing() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("Saved", "off");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    expect(queue.empty(), "the off channel queues nothing");
    expect(!behavior.outputs()[0].boolean, "and reports nothing was announced");
}

// A component that re-renders must not make a screen reader repeat itself.
void test_repeated_message_is_not_announced() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("Saved");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    expect(queue.size() == 1, "the first message is queued");

    behavior.update(ArgsView{args});
    expect(queue.size() == 1, "the same message is not queued twice");
    expect(!behavior.outputs()[0].boolean, "and the output reports it was not announced again");

    auto changed = options("Saved again");
    behavior.update(ArgsView{changed});
    expect(queue.size() == 2, "a different message is queued");
}

void test_no_queue_announces_nothing() {
    Announce behavior;
    auto args = options("Saved");
    behavior.attach(Targets{}, Services{}, ArgsView{args});
    expect(!behavior.outputs()[0].boolean, "a build with no queue announces nothing");
    expect(behavior.queue() == nullptr, "and holds no queue");
}

void test_empty_message_announces_nothing() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    expect(queue.empty(), "an empty message queues nothing");
}

void test_detach_resets() {
    AnnouncementQueue queue;
    Announce behavior;
    auto args = options("Saved");
    behavior.attach(Targets{}, Services{.announcements = &queue}, ArgsView{args});
    behavior.detach();
    expect(behavior.queue() == nullptr, "a detached behavior holds no queue");
    expect(!behavior.outputs()[0].boolean, "a detached behavior reports the manifest default");
    // The queue keeps the message: an announcement outlives the component that produced it.
    expect(queue.size() == 1, "the queued message survives the detach");
}

} // namespace

int main() {
    test_attach_queues_the_message();
    test_assertive_channel();
    test_off_channel_queues_nothing();
    test_repeated_message_is_not_announced();
    test_no_queue_announces_nothing();
    test_empty_message_announces_nothing();
    test_detach_resets();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
