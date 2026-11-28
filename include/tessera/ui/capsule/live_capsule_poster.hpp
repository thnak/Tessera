// SPDX-License-Identifier: MIT

#pragma once

// LiveCapsulePoster (docs/specs/live-capsule-v1.md section 7, issue #968): start, update and end
// of a LiveCapsuleHost from any thread. Each call copies its arguments into a completion-queue task
// that the UI thread runs at its next drain, so the host itself stays UI-thread only. The queue is
// bounded: a call returns false when the queue refused the task (full), and nothing was applied.
//
// Lifetime: the host must outlive every queued task. Drain or clear the queue before destroying the
// host.

#include "tessera/runtime/async.hpp"
#include "tessera/ui/capsule/live_capsule_host.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::capsule {

class LiveCapsulePoster {
public:
    // Both are borrowed. Capsule changes are user-visible, so they ride the `Visible` lane.
    LiveCapsulePoster(LiveCapsuleHost& host, runtime::CompletionQueue& queue) noexcept
        : host_(&host),
          queue_(&queue) {}

    // Any thread. The posted call has the semantics of the host method of the same name; its
    // result (for example an unknown id) is dropped because the poster cannot wait for it.
    bool start(Activity activity);
    bool update(std::string_view id, Slots slots);
    bool end(std::string_view id);

private:
    LiveCapsuleHost* host_;
    runtime::CompletionQueue* queue_;
};

} // namespace tessera::ui::capsule
