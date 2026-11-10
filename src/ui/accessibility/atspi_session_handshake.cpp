// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_session.hpp"

#include <string>
#include <utility>

namespace tessera::ui::a11y {
namespace {

SessionError map_address_error(AddressError error) {
    switch (error) {
    case AddressError::UnsupportedTransport:
        // A tcp:/launchd:/autolaunch: bus is not something this client can speak; the caller
        // disables accessibility and keeps running (section 7).
        return SessionError::Disabled;
    case AddressError::Empty:
    case AddressError::MissingKey:
        return SessionError::AddressMissing;
    case AddressError::None:
    case AddressError::TooLarge:
    case AddressError::BadTransport:
    case AddressError::BadKey:
    case AddressError::UnsupportedKey:
    case AddressError::BadValue:
    case AddressError::PathTooLong:
        return SessionError::AddressInvalid;
    }
    return SessionError::AddressInvalid;
}

SessionError map_auth_error(AuthError error) {
    switch (error) {
    case AuthError::Rejected:
        return SessionError::AuthRejected;
    case AuthError::Protocol:
        return SessionError::AuthProtocol;
    case AuthError::None:
    case AuthError::IdentityTooLong:
    case AuthError::ServerError:
    case AuthError::TooLarge:
        return SessionError::AuthFailed;
    }
    return SessionError::AuthFailed;
}

std::uint32_t read_u32(const std::uint8_t* bytes, bool big_endian) {
    if (big_endian) {
        return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
               (static_cast<std::uint32_t>(bytes[1]) << 16U) |
               (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
    }
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

} // namespace

std::expected<void, SessionError> AtspiSession::connect_session_bus() {
    const auto address = session_bus_address(config_.environment);
    if (!address) {
        return std::unexpected(fail(map_address_error(address.error())));
    }
    const auto opened = open_transport(*address);
    if (!opened) {
        return opened;
    }
    auth_ = std::make_unique<AuthClient>(external_identity(current_uid()));
    const auto request = auth_->initial_request();
    if (!request) {
        return std::unexpected(fail(map_auth_error(request.error())));
    }
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(request->data()), request->size());
    append_output(bytes);
    phase_.store(SessionPhase::SessionConnect);
    if (!handshake_started_) {
        handshake_started_ = true;
        handshake_started_ms_ = steady_ms();
    }
    return {};
}

std::expected<void, SessionError> AtspiSession::connect_a11y_bus(std::string address) {
    // The session bus was only needed to learn where the accessibility bus lives; AT-SPI2 objects
    // are served on the accessibility bus only, so the first connection is released here.
    close_transport();
    in_.clear();
    in_offset_ = 0;
    const auto parsed = parse_bus_address(address, config_.environment.runtime_dir);
    if (!parsed) {
        return std::unexpected(fail(map_address_error(parsed.error())));
    }
    on_a11y_bus_ = true;
    const auto opened = open_transport(*parsed);
    if (!opened) {
        return opened;
    }
    auth_ = std::make_unique<AuthClient>(external_identity(current_uid()));
    const auto request = auth_->initial_request();
    if (!request) {
        return std::unexpected(fail(map_auth_error(request.error())));
    }
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t*>(request->data()), request->size());
    append_output(bytes);
    phase_.store(SessionPhase::A11yConnect);
    return {};
}

std::expected<void, SessionError> AtspiSession::open_transport(const BusAddress& address) {
    auto transport = provider_->create();
    if (!transport) {
        return std::unexpected(fail(SessionError::Disabled));
    }
    const auto connected = transport->connect(address);
    if (!connected) {
        return std::unexpected(fail(SessionError::ConnectFailed));
    }
    transport_ = std::move(transport);
    return {};
}

std::expected<void, SessionError> AtspiSession::consume_auth(std::span<const std::uint8_t> bytes,
                                                             PumpResult& /*result*/) {
    if (!auth_) {
        return std::unexpected(fail(SessionError::Protocol));
    }
    const auto step = auth_->consume(bytes, out_);
    if (!step) {
        return std::unexpected(fail(map_auth_error(step.error())));
    }
    if (*step != AuthPhase::Complete) {
        return {};
    }
    // `BEGIN` is already in `out_`; the binary protocol starts with Hello on this same connection.
    return start_binary_phase();
}

std::expected<void, SessionError> AtspiSession::start_binary_phase() {
    auth_.reset();
    if (on_a11y_bus_) {
        phase_.store(SessionPhase::A11yHello);
    } else {
        phase_.store(SessionPhase::SessionHello);
    }
    return send_hello();
}

std::expected<std::size_t, SessionError> AtspiSession::frame_size() const {
    const std::size_t available = in_.size() - in_offset_;
    if (available < 16) {
        return std::size_t{0};
    }
    const std::uint8_t* const header = in_.data() + in_offset_;
    bool big_endian = false;
    if (header[0] == 'l') {
        big_endian = false;
    } else if (header[0] == 'B') {
        big_endian = true;
    } else {
        return std::unexpected(SessionError::Protocol);
    }
    const std::size_t body_length = read_u32(header + 4, big_endian);
    const std::size_t fields_length = read_u32(header + 12, big_endian);
    if (fields_length > kMaxWireFieldBytes || body_length > kMaxWireBody) {
        return std::unexpected(SessionError::Protocol);
    }
    // The body starts at the first 8-byte boundary after the header fields (D-Bus alignment).
    const std::size_t body_start = (16 + fields_length + 7U) & ~std::size_t{7U};
    const std::size_t total = body_start + body_length;
    if (total > kMaxWireMessage) {
        return std::unexpected(SessionError::Protocol);
    }
    return total;
}

std::expected<void, SessionError> AtspiSession::consume_binary(PumpResult& result) {
    std::size_t framed = 0;
    while (framed < kMaxMessagesPerPump) {
        const auto total = frame_size();
        if (!total) {
            return std::unexpected(fail(total.error()));
        }
        const std::size_t available = in_.size() - in_offset_;
        if (*total == 0 || available < *total) {
            break; // a partial message waits for the next read
        }
        const std::span<const std::uint8_t> raw(in_.data() + in_offset_, *total);
        const auto parsed = parse_message(raw);
        if (!parsed) {
            return std::unexpected(fail(SessionError::Protocol));
        }
        const auto handled = handle_message(*parsed, raw, result);
        if (!handled) {
            return std::unexpected(handled.error());
        }
        // A handler may switch connections (the address reply opens the accessibility bus) and
        // drop the bytes buffered for the old one; only advance when the buffer is still ours.
        if (in_.size() >= in_offset_ + *total) {
            in_offset_ += *total;
        } else {
            in_offset_ = 0;
        }
        ++result.messages;
        ++framed;
    }
    if (in_offset_ >= in_.size()) {
        in_.clear();
        in_offset_ = 0;
    } else if (in_offset_ > 0) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(in_offset_));
        in_offset_ = 0;
    }
    return {};
}

std::expected<void, SessionError> AtspiSession::handle_message(const ParsedMessage& message,
                                                               std::span<const std::uint8_t> raw,
                                                               PumpResult& result) {
    switch (message.header.type) {
    case MessageType::MethodReturn:
        return handle_reply(message.header, message.body, result);
    case MessageType::Error: {
        if (find_call(message.header.reply_serial) == nullptr) {
            return std::unexpected(fail(SessionError::UnexpectedReply));
        }
        {
            std::lock_guard lock(state_mutex_);
            bus_error_name_ = message.header.error_name;
        }
        return std::unexpected(fail(SessionError::BusError));
    }
    case MessageType::MethodCall: {
        // Assistive technology can only reach us after the registry hands out our bus name, so a
        // call before `Ready` means the peer is not the bus we handshook with.
        if (phase() != SessionPhase::Ready) {
            return std::unexpected(fail(SessionError::Protocol));
        }
        const auto dispatched = bridge_.dispatch(raw, out_);
        if (!dispatched) {
            return std::unexpected(fail(SessionError::Protocol));
        }
        ++result.dispatched;
        return {};
    }
    case MessageType::Signal:
        // NameAcquired/NameLost and other daemon chatter are informational; no reply is owed.
        return {};
    }
    return std::unexpected(fail(SessionError::Protocol));
}

std::expected<void, SessionError> AtspiSession::handle_reply(const MessageHeader& header,
                                                             std::span<const std::uint8_t> body,
                                                             PumpResult& /*result*/) {
    PendingCall* const call = find_call(header.reply_serial);
    if (call == nullptr) {
        return std::unexpected(fail(SessionError::UnexpectedReply));
    }
    const CallKind kind = call->kind;
    const std::size_t position = static_cast<std::size_t>(call - calls_.data());
    calls_[position] = calls_[call_count_ - 1];
    --call_count_;

    switch (kind) {
    case CallKind::Hello: {
        MessageReader reader(body);
        const std::string_view name = reader.read_string();
        if (reader.failed() || name.empty()) {
            return std::unexpected(fail(SessionError::Protocol));
        }
        {
            std::lock_guard lock(state_mutex_);
            unique_name_ = std::string(name);
        }
        bridge_.set_bus_name(std::string(name));
        if (!on_a11y_bus_) {
            phase_.store(SessionPhase::A11yAddress);
            return send_get_address();
        }
        if (!config_.register_events) {
            phase_.store(SessionPhase::Ready);
            return {};
        }
        phase_.store(SessionPhase::Registering);
        return send_register_event();
    }
    case CallKind::GetA11yAddress: {
        MessageReader reader(body);
        const std::string_view address = reader.read_string();
        if (reader.failed() || address.empty()) {
            return std::unexpected(fail(SessionError::Protocol));
        }
        {
            std::lock_guard lock(state_mutex_);
            a11y_address_ = std::string(address);
        }
        // `Application.GetApplicationBusAddress` reports the address ATs should connect to.
        bridge_.set_bus_address(std::string(address));
        return connect_a11y_bus(std::string(address));
    }
    case CallKind::RegisterEvent:
        // The registry answers with an empty method return once the event mask is recorded.
        phase_.store(SessionPhase::Ready);
        return {};
    }
    return std::unexpected(fail(SessionError::Protocol));
}

std::expected<void, SessionError> AtspiSession::send_hello() {
    return send_call(CallKind::Hello, kSessionBusName, kSessionBusPath, kSessionBusInterface,
                     kSessionBusMember, std::string_view{});
}

std::expected<void, SessionError> AtspiSession::send_get_address() {
    return send_call(CallKind::GetA11yAddress, kA11yBusName, kA11yBusPath, kA11yBusName,
                     kA11yBusMember, std::string_view{});
}

std::expected<void, SessionError> AtspiSession::send_register_event() {
    return send_call(CallKind::RegisterEvent, kRegistryBusName, kRegistryPath, kRegistryInterface,
                     kRegistryMember, "as");
}

std::expected<void, SessionError>
AtspiSession::send_call(CallKind kind, std::string_view destination, std::string_view path,
                        std::string_view interface, std::string_view member,
                        std::string_view signature) {
    if (call_count_ >= kMaxPendingCalls) {
        return std::unexpected(fail(SessionError::TooManyCalls));
    }
    MessageHeader header;
    header.type = MessageType::MethodCall;
    header.serial = bridge_.next_serial();
    header.destination = std::string(destination);
    header.path = std::string(path);
    header.interface = std::string(interface);
    header.member = std::string(member);
    header.signature = std::string(signature);
    const auto begun = writer_.begin_message(header);
    if (!begun) {
        return std::unexpected(fail(SessionError::Protocol));
    }
    if (kind == CallKind::RegisterEvent) {
        writer_.write_array('s', [](MessageWriter& array) {
            for (const std::string_view name : kEventRegistrations) {
                array.write_string(name);
            }
        });
    }
    const auto ended = writer_.end_message();
    if (!ended) {
        return std::unexpected(fail(SessionError::Protocol));
    }
    calls_[call_count_] = PendingCall{header.serial, kind};
    ++call_count_;
    append_output(writer_.bytes());
    return {};
}

AtspiSession::PendingCall* AtspiSession::find_call(std::uint32_t serial) {
    for (std::size_t index = 0; index < call_count_; ++index) {
        if (calls_[index].serial == serial) {
            return &calls_[index];
        }
    }
    return nullptr;
}

void AtspiSession::clear_calls() {
    call_count_ = 0;
}

} // namespace tessera::ui::a11y
