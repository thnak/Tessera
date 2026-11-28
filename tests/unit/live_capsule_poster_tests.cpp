// SPDX-License-Identifier: MIT
//
// LiveCapsulePoster (issue #968, docs/specs/live-capsule-v1.md section 7): start, update and end
// posted from another thread take effect only when the UI thread drains the completion queue.

#include "tessera/runtime/async.hpp"
#include "tessera/ui/capsule/live_capsule_poster.hpp"

#include <iostream>
#include <thread>

namespace {

using namespace tessera::ui::capsule;
using tessera::runtime::CompletionQueue;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "live_capsule_poster: " << message << "\n";
        ++failures;
    }
    return condition;
}

Activity timer() {
    Activity a;
    a.id = "timer";
    a.label = "Timer running";
    a.slots.title = "one";
    return a;
}

void test_cross_thread_calls_apply_on_drain() {
    LiveCapsuleHost host;
    CompletionQueue queue;
    LiveCapsulePoster poster(host, queue);

    bool accepted = false;
    std::thread worker([&] {
        accepted = poster.start(timer());
        Slots slots;
        slots.title = "two";
        accepted = poster.update("timer", std::move(slots)) && accepted;
    });
    worker.join();
    expect(accepted, "the queue accepted both calls");
    expect(host.size() == 0, "nothing is applied before the UI thread drains");

    expect(queue.run(8) == 2, "the drain runs both calls");
    expect(host.size() == 1 && host.find("timer") != nullptr &&
               host.find("timer")->slots.title == "two",
           "start then update arrive in post order");

    std::thread ender([&] { accepted = poster.end("timer"); });
    ender.join();
    expect(accepted && host.size() == 1, "the end waits for the drain too");
    queue.run(8);
    expect(host.size() == 0, "the posted end removes the activity");
}

void test_unknown_ids_and_full_queue() {
    LiveCapsuleHost host;
    CompletionQueue queue(1);
    LiveCapsulePoster poster(host, queue);
    expect(poster.end("ghost"), "posting an end for an unknown id is accepted");
    expect(!poster.start(timer()), "a full queue refuses the call");
    queue.run(8);
    expect(host.size() == 0, "the refused start was never applied");
    expect(poster.start(timer()), "the queue accepts again after a drain");
    queue.run(8);
    expect(host.size() == 1, "and that start applies");
}

} // namespace

int main() {
    test_cross_thread_calls_apply_on_drain();
    test_unknown_ids_and_full_queue();
    if (failures != 0) {
        std::cerr << "live_capsule_poster: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "live_capsule_poster: ok\n";
    return 0;
}
