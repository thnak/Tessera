// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace tessera::ui::a11y {

// D-Bus server address parsing (docs/specs/accessibility-v1.md section 7). The AT-SPI2 bridge
// speaks the D-Bus wire protocol directly (ADR-012: no libatspi/glib), so it has to resolve the
// session-bus address, and the accessibility bus address the registry hands back, into a concrete
// UNIX socket endpoint itself.
//
// Only the `unix:` transport is implemented: `path=` for a filesystem socket and `abstract=` for
// the Linux abstract namespace. Every other transport (`tcp:`, `nonce-tcp:`, `unixexec:`,
// `launchd:`, `autolaunch:`) reports `UnsupportedTransport` instead of guessing, so the caller can
// disable accessibility with a diagnostic and keep running (section 7: bridges are optional).
//
// Parsing is deterministic and heap-bounded: at most kMaxAddressBytes of input, at most
// kMaxUnixPathBytes per endpoint, and no environment reads. `read_session_environment()` is the one
// place that touches the process environment.
enum class AddressError : std::uint8_t {
    None = 0,
    Empty,                // no address text at all
    TooLarge,             // the address text exceeds kMaxAddressBytes
    BadTransport,         // the transport name is empty or has invalid characters
    UnsupportedTransport, // a well-formed transport this client does not implement
    BadKey,               // a parameter without `key=value` shape
    UnsupportedKey,       // a key that is not part of the unix transport
    BadValue,             // invalid escape, control byte, duplicate or conflicting source
    MissingKey,           // no path/abstract/runtime source, or `runtime=yes` without a directory
    PathTooLong,          // the decoded endpoint exceeds kMaxUnixPathBytes
};

// Longest accepted address string. Real session addresses are well under 200 bytes; the cap keeps a
// hostile `DBUS_SESSION_BUS_ADDRESS` from turning into unbounded work.
inline constexpr std::size_t kMaxAddressBytes = 1024;
// `sockaddr_un::sun_path` is 108 bytes including the terminating NUL, so 107 payload bytes fit.
inline constexpr std::size_t kMaxUnixPathBytes = 107;

enum class BusTransport : std::uint8_t {
    UnixPath,     // unix:path=...
    UnixAbstract, // unix:abstract=...
};

// A resolved UNIX endpoint. `path` is the filesystem path, or the abstract namespace name without
// its leading NUL (the socket layer adds that).
struct BusAddress {
    BusTransport transport = BusTransport::UnixPath;
    std::string path;

    [[nodiscard]] bool abstract() const {
        return transport == BusTransport::UnixAbstract;
    }
};

// Values used to expand `unix:runtime=yes` and to fall back to the default session socket. Kept as
// explicit inputs so tests never depend on the process environment; `read_session_environment`
// copies the process values into owned strings for callers that do want them.
struct SessionEnvironment {
    std::string session_bus_address; // DBUS_SESSION_BUS_ADDRESS
    std::string runtime_dir;         // XDG_RUNTIME_DIR
};

// Reads DBUS_SESSION_BUS_ADDRESS and XDG_RUNTIME_DIR from the process environment. The strings are
// owned copies, so later environment mutations cannot invalidate what the bridge connected to.
[[nodiscard]] SessionEnvironment read_session_environment();

// Parses an address list (`;`-separated) and returns the first usable UNIX endpoint. Later entries
// are only consulted when an earlier one is unsupported, so a list such as
// `tcp:host=localhost,port=1;unix:path=/tmp/bus` resolves to the working local socket.
[[nodiscard]] std::expected<BusAddress, AddressError>
parse_bus_address(std::string_view text, std::string_view runtime_dir = {});

// Resolves the session bus: the explicit DBUS_SESSION_BUS_ADDRESS when set, else the
// XDG_RUNTIME_DIR/bus default of modern Linux systems.
[[nodiscard]] std::expected<BusAddress, AddressError>
session_bus_address(const SessionEnvironment& environment);

// Inverse of parsing, used by diagnostics and round-trip tests. Byte values outside the unescaped
// set are percent-encoded, so `parse_bus_address(format_bus_address(x)) == x`.
[[nodiscard]] std::string format_bus_address(const BusAddress& address);

// Stable lowercase names for diagnostics and test failures.
[[nodiscard]] std::string_view to_string(AddressError error);

} // namespace tessera::ui::a11y
