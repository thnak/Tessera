// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/live_capsule_poster.hpp"

#include <utility>

namespace tessera::ui::capsule {

bool LiveCapsulePoster::start(Activity activity) {
    // std::function needs a copyable callable, so the activity is held by value in the lambda.
    return queue_->post(
        runtime::CompletionPriority::Visible,
        [host = host_, activity = std::move(activity)]() { (void)host->start(activity); });
}

bool LiveCapsulePoster::update(std::string_view id, Slots slots) {
    return queue_->post(runtime::CompletionPriority::Visible,
                        [host = host_, id = std::string(id), slots = std::move(slots)]() {
                            (void)host->update(id, slots);
                        });
}

bool LiveCapsulePoster::end(std::string_view id) {
    return queue_->post(runtime::CompletionPriority::Visible,
                        [host = host_, id = std::string(id)]() { (void)host->end(id); });
}

} // namespace tessera::ui::capsule
