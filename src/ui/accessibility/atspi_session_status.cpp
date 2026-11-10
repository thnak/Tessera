// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/atspi_session.hpp"

#include <string_view>

namespace tessera::ui::a11y {

std::string_view to_string(SessionError error) {
    switch (error) {
    case SessionError::None:
        return "none";
    case SessionError::Disabled:
        return "disabled";
    case SessionError::AddressMissing:
        return "address missing";
    case SessionError::AddressInvalid:
        return "address invalid";
    case SessionError::ConnectFailed:
        return "connect failed";
    case SessionError::AuthRejected:
        return "auth rejected";
    case SessionError::AuthProtocol:
        return "auth protocol";
    case SessionError::AuthFailed:
        return "auth failed";
    case SessionError::TransportClosed:
        return "transport closed";
    case SessionError::TransportFailure:
        return "transport failure";
    case SessionError::Protocol:
        return "protocol";
    case SessionError::UnexpectedReply:
        return "unexpected reply";
    case SessionError::BusError:
        return "bus error";
    case SessionError::Timeout:
        return "timeout";
    case SessionError::TooManyCalls:
        return "too many calls";
    case SessionError::ThreadFailed:
        return "thread failed";
    case SessionError::NotRunning:
        return "not running";
    case SessionError::SyncTimeout:
        return "sync timeout";
    }
    return "unknown";
}

std::string_view to_string(SessionPhase phase) {
    switch (phase) {
    case SessionPhase::Idle:
        return "idle";
    case SessionPhase::SessionConnect:
        return "session-connect";
    case SessionPhase::SessionHello:
        return "session-hello";
    case SessionPhase::A11yAddress:
        return "a11y-address";
    case SessionPhase::A11yConnect:
        return "a11y-connect";
    case SessionPhase::A11yHello:
        return "a11y-hello";
    case SessionPhase::Registering:
        return "registering";
    case SessionPhase::Ready:
        return "ready";
    case SessionPhase::Closed:
        return "closed";
    case SessionPhase::Failed:
        return "failed";
    }
    return "unknown";
}

SessionConfig session_config_from_environment() {
    SessionConfig config;
    config.environment = read_session_environment();
    return config;
}

std::unique_ptr<Transport> UnixTransportProvider::create() {
    return create_unix_socket_transport();
}

} // namespace tessera::ui::a11y
