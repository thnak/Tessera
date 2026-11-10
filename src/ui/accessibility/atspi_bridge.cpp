// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_bridge.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace tessera::ui::a11y {
namespace {

constexpr std::string_view kUnknownObject = "org.freedesktop.DBus.Error.UnknownObject";
constexpr std::string_view kUnknownMethod = "org.freedesktop.DBus.Error.UnknownMethod";
constexpr std::string_view kEventInterface = "org.a11y.atspi.Event.Object";

std::string path_for(A11yIndex index) {
    std::string path(kAccessiblePathPrefix);
    path.append(std::to_string(index));
    return path;
}

} // namespace

AtspiBridge::AtspiBridge(BridgeConfig config)
    : config_(std::move(config)) {
    snapshot_.nodes.reserve(kMaxNodes);
}

void AtspiBridge::publish(const A11ySnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    // A plain copy keeps the bridge thread free of scene/shared_ptr lifetime rules; the builder
    // already bounds the snapshot at kMaxNodes, and publishing happens on dirty changes, not
    // frames.
    snapshot_.nodes.assign(snapshot.nodes.begin(), snapshot.nodes.end());
}

bool AtspiBridge::post_event(const A11yEvent& event) {
    return events_.push(event);
}

std::size_t AtspiBridge::take_actions(std::span<ActionRequest> out) {
    std::lock_guard lock(mutex_);
    const std::size_t taken = std::min(out.size(), action_count_);
    for (std::size_t index = 0; index < taken; ++index) {
        out[index] = actions_[(action_head_ + index) % kMaxPendingActions];
    }
    action_head_ = (action_head_ + taken) % kMaxPendingActions;
    action_count_ -= taken;
    return taken;
}

std::expected<void, WireError> AtspiBridge::dispatch(std::span<const std::uint8_t> message,
                                                     std::vector<std::uint8_t>& out) {
    const auto parsed = parse_message(message);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    // Replies and signals on this connection are not ours to answer; the bridge only serves calls.
    if (parsed->header.type != MessageType::MethodCall) {
        return {};
    }
    std::lock_guard lock(mutex_);
    handle_call(*parsed, out);
    return {};
}

void AtspiBridge::handle_call(const ParsedMessage& call, std::vector<std::uint8_t>& out) {
    const A11yIndex index = parse_object_path(call.header.path);
    if (index == kInvalidIndex || static_cast<std::size_t>(index) >= snapshot_.nodes.size()) {
        start_error(call.header, kUnknownObject, "no such accessible");
        (void)finish_reply(out);
        return;
    }
    const std::string_view iface = call.header.interface;
    bool handled = false;
    if (iface.empty() || iface == kAccessibleInterface) {
        handled = handle_accessible(call, index, out);
    } else if (iface == kApplicationInterface) {
        handled = handle_application(call, out);
    } else if (iface == kActionInterface) {
        handled = handle_action(call, index, out);
    } else if (iface == kComponentInterface) {
        handled = handle_component(call, index, out);
    } else if (iface == kTextInterface) {
        handled = handle_text(call, index, out);
    } else if (iface == kValueInterface) {
        handled = handle_value(call, index, out);
    }
    if (!handled) {
        start_error(call.header, kUnknownMethod, "unsupported method");
        (void)finish_reply(out);
    }
}

std::size_t AtspiBridge::emit_events(std::vector<std::uint8_t>& out, std::size_t max_events) {
    std::lock_guard lock(mutex_);
    std::array<A11yEvent, kMaxEventBatch> batch{};
    const std::size_t limit = std::min({max_events, batch.size(), kMaxEventBatch});
    const std::size_t taken = events_.take(std::span<A11yEvent>(batch.data(), limit));
    for (std::size_t index = 0; index < taken; ++index) {
        write_event_signal(batch[index], out);
    }
    return taken;
}

void AtspiBridge::set_bus_name(std::string name) {
    std::lock_guard lock(mutex_);
    bus_name_ = std::move(name);
}

std::string_view AtspiBridge::bus_name() const {
    std::lock_guard lock(mutex_);
    return bus_name_;
}

std::uint32_t AtspiBridge::next_serial() {
    const std::uint32_t serial = serial_.fetch_add(1, std::memory_order_relaxed);
    // D-Bus reserves serial 0; skipping it keeps a long-lived bridge valid even after a wrap.
    if (serial == 0) {
        return serial_.fetch_add(1, std::memory_order_relaxed);
    }
    return serial;
}

std::size_t AtspiBridge::node_count() const {
    std::lock_guard lock(mutex_);
    return snapshot_.nodes.size();
}

std::size_t AtspiBridge::pending_events() const {
    return events_.size();
}

std::size_t AtspiBridge::coalesced_events() const {
    return events_.coalesced();
}

std::size_t AtspiBridge::dropped_events() const {
    return events_.dropped();
}

MessageWriter& AtspiBridge::start_reply(const MessageHeader& call, std::string_view signature) {
    writer_.reset();
    MessageHeader header;
    header.type = MessageType::MethodReturn;
    header.serial = next_serial();
    header.reply_serial = call.serial;
    header.destination = call.sender;
    header.signature = std::string(signature);
    (void)writer_.begin_message(header);
    return writer_;
}

void AtspiBridge::start_error(const MessageHeader& call, std::string_view name,
                              std::string_view text) {
    writer_.reset();
    MessageHeader header;
    header.type = MessageType::Error;
    header.serial = next_serial();
    header.reply_serial = call.serial;
    header.destination = call.sender;
    header.error_name = std::string(name);
    header.signature = "s";
    (void)writer_.begin_message(header);
    writer_.write_string(text);
}

std::expected<void, WireError> AtspiBridge::finish_reply(std::vector<std::uint8_t>& out) {
    const auto finished = writer_.end_message();
    if (!finished) {
        return finished;
    }
    const auto bytes = writer_.bytes();
    out.insert(out.end(), bytes.begin(), bytes.end());
    return {};
}

void AtspiBridge::write_reference(MessageWriter& writer, A11yIndex index) {
    writer.begin_struct();
    if (index == kInvalidIndex) {
        // An empty bus name is AT-SPI2's null reference; the path is a stable, meaningless
        // placeholder.
        writer.write_string("");
        writer.write_object_path(kNullObjectPath);
    } else {
        writer.write_string(bus_name_);
        writer.write_object_path(path_for(index));
    }
    writer.end_struct();
}

A11yIndex AtspiBridge::parent_of(A11yIndex index) const {
    for (std::size_t candidate = 0; candidate < snapshot_.nodes.size(); ++candidate) {
        const A11yNode& node = snapshot_.nodes[candidate];
        for (std::uint16_t child = 0; child < node.child_count; ++child) {
            if (node.children[child] == index) {
                return static_cast<A11yIndex>(candidate);
            }
        }
    }
    return kInvalidIndex;
}

bool AtspiBridge::queue_action(const ActionRequest& request) {
    // Identical (node, action) requests coalesce: a screen reader that clicks twice before the next
    // tick gets one command, never a burst.
    for (std::size_t offset = 0; offset < action_count_; ++offset) {
        ActionRequest& queued = actions_[(action_head_ + offset) % kMaxPendingActions];
        if (queued.node == request.node && queued.action == request.action) {
            queued.value = request.value;
            queued.action_index = request.action_index;
            return true;
        }
    }
    if (action_count_ >= kMaxPendingActions) {
        return false;
    }
    actions_[(action_head_ + action_count_) % kMaxPendingActions] = request;
    ++action_count_;
    return true;
}

void AtspiBridge::write_event_signal(const A11yEvent& event, std::vector<std::uint8_t>& out) {
    if (event.node == kInvalidIndex ||
        static_cast<std::size_t>(event.node) >= snapshot_.nodes.size()) {
        return;
    }
    writer_.reset();
    MessageHeader header;
    header.type = MessageType::Signal;
    header.serial = next_serial();
    header.path = path_for(event.node);
    header.interface = std::string(kEventInterface);
    header.member = std::string(event_signal_member(event.kind));
    header.signature = "siiva{sv}";
    if (!writer_.begin_message(header)) {
        return;
    }
    const std::string detail(event_detail(event));
    writer_.write_string(detail);
    writer_.write_int32(event.detail1);
    writer_.write_int32(event.detail2);
    writer_.write_variant(
        "s", [&event](MessageWriter& value) { value.write_string(event_value(event)); });
    writer_.write_array('{', [](MessageWriter&) {});
    if (!writer_.end_message()) {
        return;
    }
    const auto bytes = writer_.bytes();
    out.insert(out.end(), bytes.begin(), bytes.end());
}

} // namespace tessera::ui::a11y
