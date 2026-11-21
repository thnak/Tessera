// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/announce.hpp"

#include <string_view>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `message`, `channel`.
constexpr std::string_view kOptionNames[] = {"message", "channel"};

accessibility::LiveChannel channel_of(std::string_view name) {
    if (name == "assertive") {
        return accessibility::LiveChannel::Assertive;
    }
    if (name == "off") {
        return accessibility::LiveChannel::Off;
    }
    return accessibility::LiveChannel::Polite;
}

} // namespace

void Announce::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    (void)targets; // an announcement has no element: it is a message, not content
    queue_ = services.announcements;
    const ArgValue* message = args.find("message", kOptionNames);
    const ArgValue* channel = args.find("channel", kOptionNames);
    apply(message != nullptr && message->kind == ArgValue::Kind::String ? message->text
                                                                        : std::string_view{},
          channel != nullptr && channel->kind == ArgValue::Kind::String
              ? channel_of(channel->text)
              : accessibility::LiveChannel::Polite);
}

void Announce::update(const ArgsView& args) {
    const ArgValue* message = args.find("message", kOptionNames);
    const ArgValue* channel = args.find("channel", kOptionNames);
    apply(message != nullptr && message->kind == ArgValue::Kind::String ? message->text
                                                                        : std::string_view{},
          channel != nullptr && channel->kind == ArgValue::Kind::String
              ? channel_of(channel->text)
              : accessibility::LiveChannel::Polite);
}

void Announce::detach() {
    queue_ = nullptr;
    last_.clear();
    outputs_ = {};
}

std::span<const OutputValue> Announce::outputs() const {
    return outputs_;
}

void Announce::apply(std::string_view message, accessibility::LiveChannel channel) {
    outputs_[0].kind = OutputValue::Kind::Bool;
    if (queue_ == nullptr || message.empty() || channel == accessibility::LiveChannel::Off) {
        outputs_[0].boolean = false;
        return;
    }
    // A repeated message is not a new announcement: a component that re-renders must not make a
    // screen reader repeat itself.
    if (last_ == message) {
        outputs_[0].boolean = false;
        return;
    }
    const bool queued = queue_->announce(channel, message);
    if (queued) {
        last_.assign(message);
    }
    outputs_[0].boolean = queued;
}

} // namespace tessera::ui::behavior
