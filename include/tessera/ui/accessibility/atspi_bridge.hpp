// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/atspi_events.hpp"
#include "tessera/ui/accessibility/atspi_mapping.hpp"
#include "tessera/ui/accessibility/dbus_wire.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::a11y {

// AT-SPI2 bridge core (docs/specs/accessibility-v1.md section 7, docs/design/accessibility.md
// section 3). It turns the immutable semantics snapshot into the six interfaces an assistive
// technology calls - Accessible, Application, Action, Component, Text, Value - and turns queued
// semantic events into `org.a11y.atspi.Event.Object` signals.
//
// Threading (ADR-021): the UI thread only calls `publish` (a bounded snapshot copy) and
// `post_event` (a bounded queue push); both take a mutex for a few microseconds and never touch
// I/O. `dispatch`/`emit_events` run on the bridge thread, where the caller also pumps the
// transport. Actions never run here: `DoAction`/`GrabFocus`/`SetCurrentValue` validate and append
// an `ActionRequest`, and the UI thread drains them at a tick boundary (`take_actions`).
struct ActionRequest {
    A11yIndex node = kInvalidIndex;
    Action action = Action::None;
    std::uint32_t action_index = 0;
    double value = 0.0; // SetValue payload (unused for the other actions)
};

inline constexpr std::size_t kMaxPendingActions = 32;
// Largest event batch one `emit_events` call may serialize, keeping a single pump iteration
// bounded.
inline constexpr std::size_t kMaxEventBatch = 8;

struct BridgeConfig {
    std::string application_name = "Tessera";
    std::string application_version = "0.1";
    std::string toolkit_name = "Tessera";
    std::string locale = "en";
    // The accessibility bus address reported by `GetApplicationBusAddress`; the caller fills it in
    // once the registry has answered.
    std::string bus_address;
};

// Object path of snapshot index 0 comes from `kRootObjectPath` (atspi_mapping.hpp): the root
// object also carries the Application interface.

class AtspiBridge {
public:
    explicit AtspiBridge(BridgeConfig config = {});

    // UI thread: publish the latest snapshot. The copy is bounded by kMaxNodes and does not
    // allocate per node beyond one vector.
    void publish(const A11ySnapshot& snapshot);
    // UI thread: queue a semantic event (bounded, coalescing).
    [[nodiscard]] bool post_event(const A11yEvent& event);
    // UI thread: drain action requests that arrived from assistive technology.
    [[nodiscard]] std::size_t take_actions(std::span<ActionRequest> out);

    // Bridge thread: answer one incoming message. Replies are appended to `out`; messages that are
    // not method calls (replies, signals) are ignored. A framing failure is returned, not thrown.
    [[nodiscard]] std::expected<void, WireError> dispatch(std::span<const std::uint8_t> message,
                                                          std::vector<std::uint8_t>& out);
    // Bridge thread: serialize up to `max_events` queued events as D-Bus signals.
    [[nodiscard]] std::size_t emit_events(std::vector<std::uint8_t>& out,
                                          std::size_t max_events = kMaxEventBatch);

    void set_bus_name(std::string name);
    [[nodiscard]] std::string_view bus_name() const;
    // The accessibility bus address the registry returned, reported by
    // `GetApplicationBusAddress`. `AtspiSession` fills it in after `org.a11y.Bus.GetAddress`.
    void set_bus_address(std::string address);
    [[nodiscard]] const BridgeConfig& config() const {
        return config_;
    }
    [[nodiscard]] std::uint32_t next_serial();

    [[nodiscard]] std::size_t node_count() const;
    [[nodiscard]] std::size_t pending_events() const;
    [[nodiscard]] std::size_t coalesced_events() const;
    [[nodiscard]] std::size_t dropped_events() const;

private:
    void handle_call(const ParsedMessage& call, std::vector<std::uint8_t>& out);
    bool handle_accessible(const ParsedMessage& call, A11yIndex index,
                           std::vector<std::uint8_t>& out);
    bool handle_application(const ParsedMessage& call, std::vector<std::uint8_t>& out);
    bool handle_action(const ParsedMessage& call, A11yIndex index, std::vector<std::uint8_t>& out);
    bool handle_component(const ParsedMessage& call, A11yIndex index,
                          std::vector<std::uint8_t>& out);
    bool handle_text(const ParsedMessage& call, A11yIndex index, std::vector<std::uint8_t>& out);
    bool handle_value(const ParsedMessage& call, A11yIndex index, std::vector<std::uint8_t>& out);
    // `org.freedesktop.DBus.Properties` `Get`/`GetAll` (see atspi_properties.cpp).
    bool handle_properties(const ParsedMessage& call, A11yIndex index,
                           std::vector<std::uint8_t>& out);
    // Writes the variant value for one interface property; false when the interface or the property
    // is not part of the bridge's property table.
    bool write_property_value(MessageWriter& writer, A11yIndex index, std::string_view interface,
                              std::string_view property);

    MessageWriter& start_reply(const MessageHeader& call, std::string_view signature);
    void start_error(const MessageHeader& call, std::string_view name, std::string_view text);
    [[nodiscard]] std::expected<void, WireError> finish_reply(std::vector<std::uint8_t>& out);
    // Replies InvalidArgs and reports the method as handled.
    bool reply_invalid_args(const MessageHeader& call, std::span<const std::uint8_t> body,
                            std::vector<std::uint8_t>& out);

    // Appends a `(so)` accessible reference for `index`; kInvalidIndex writes the null reference.
    void write_reference(MessageWriter& writer, A11yIndex index);
    [[nodiscard]] A11yIndex parent_of(A11yIndex index) const;
    [[nodiscard]] bool queue_action(const ActionRequest& request);
    void write_event_signal(const A11yEvent& event, std::vector<std::uint8_t>& out);

    mutable std::mutex mutex_;
    A11ySnapshot snapshot_;
    BridgeConfig config_;
    EventQueue events_;
    std::array<ActionRequest, kMaxPendingActions> actions_{};
    std::size_t action_head_ = 0;
    std::size_t action_count_ = 0;
    std::string bus_name_;
    std::string path_buffer_;
    std::atomic<std::uint32_t> serial_{1};
    MessageWriter writer_;
};

} // namespace tessera::ui::a11y
