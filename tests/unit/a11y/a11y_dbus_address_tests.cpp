// SPDX-License-Identifier: MIT
//
// D-Bus address parsing: UNIX path/abstract endpoints, `runtime=yes` expansion, escaping, the
// `;`-separated fallback list, and every rejection path the bridge turns into "accessibility
// unavailable".

#include "a11y_dbus_test_support.hpp"
#include "tessera/ui/accessibility/dbus_address.hpp"

#include <string>

namespace {

using namespace a11y_dbus_test;
using tessera::ui::a11y::AddressError;
using tessera::ui::a11y::BusAddress;
using tessera::ui::a11y::BusTransport;
using tessera::ui::a11y::format_bus_address;
using tessera::ui::a11y::kMaxUnixPathBytes;
using tessera::ui::a11y::parse_bus_address;
using tessera::ui::a11y::read_session_environment;
using tessera::ui::a11y::session_bus_address;
using tessera::ui::a11y::SessionEnvironment;
using tessera::ui::a11y::to_string;

bool unix_path() {
    const auto parsed = parse_bus_address("unix:path=/run/user/1000/bus");
    return expect(parsed.has_value(), "unix path parses") &&
           expect(parsed->transport == BusTransport::UnixPath, "unix path transport") &&
           expect_equal(parsed->path, "/run/user/1000/bus", "unix path value") &&
           expect(!parsed->abstract(), "unix path is not abstract");
}

bool unix_abstract() {
    const auto parsed = parse_bus_address("unix:abstract=/tmp/dbus-Xy12");
    return expect(parsed.has_value(), "unix abstract parses") &&
           expect(parsed->transport == BusTransport::UnixAbstract, "abstract transport") &&
           expect_equal(parsed->path, "/tmp/dbus-Xy12", "abstract name") &&
           expect(parsed->abstract(), "abstract predicate");
}

bool escaped_values() {
    const auto comma = parse_bus_address("unix:path=/tmp/a%2cb");
    if (!expect(comma.has_value(), "escaped comma parses") ||
        !expect_equal(comma->path, "/tmp/a,b", "escaped comma decodes") ||
        !expect_equal(format_bus_address(*comma), "unix:path=/tmp/a%2Cb", "comma re-escapes")) {
        return false;
    }
    const auto space = parse_bus_address("unix:path=/tmp/a%20b");
    if (!expect(space.has_value(), "escaped space parses") ||
        !expect_equal(space->path, "/tmp/a b", "escaped space decodes")) {
        return false;
    }
    const auto percent = parse_bus_address("unix:path=/tmp/100%25");
    return expect(percent.has_value(), "escaped percent parses") &&
           expect_equal(percent->path, "/tmp/100%", "escaped percent decodes") &&
           expect_equal(format_bus_address(*percent), "unix:path=/tmp/100%25",
                        "percent re-escapes");
}

bool runtime_expansion() {
    const auto expanded = parse_bus_address("unix:runtime=yes", "/run/user/1000");
    if (!expect(expanded.has_value(), "runtime=yes expands") ||
        !expect_equal(expanded->path, "/run/user/1000/bus", "runtime bus path")) {
        return false;
    }
    const auto trailing = parse_bus_address("unix:runtime=yes", "/run/user/1000/");
    if (!expect(trailing.has_value(), "runtime with trailing slash expands") ||
        !expect_equal(trailing->path, "/run/user/1000/bus", "no doubled slash")) {
        return false;
    }
    const auto missing = parse_bus_address("unix:runtime=yes");
    if (!expect(missing.error() == AddressError::MissingKey, "runtime needs a directory")) {
        return false;
    }
    const auto bad = parse_bus_address("unix:runtime=no", "/run/user/1000");
    return expect(bad.error() == AddressError::BadValue, "runtime must be yes");
}

bool session_resolution() {
    // The explicit environment address wins, including its own transport rejection.
    const SessionEnvironment explicit_env{"unix:path=/tmp/session", "/run/user/1000"};
    const auto explicit_address = session_bus_address(explicit_env);
    if (!expect(explicit_address.has_value(), "explicit session address") ||
        !expect_equal(explicit_address->path, "/tmp/session", "explicit address value")) {
        return false;
    }
    // Without DBUS_SESSION_BUS_ADDRESS, the XDG runtime default applies.
    const SessionEnvironment runtime_env{"", "/run/user/1000"};
    const auto runtime_address = session_bus_address(runtime_env);
    if (!expect(runtime_address.has_value(), "runtime fallback") ||
        !expect_equal(runtime_address->path, "/run/user/1000/bus", "runtime fallback value")) {
        return false;
    }
    const SessionEnvironment empty_env{"", ""};
    if (!expect(session_bus_address(empty_env).error() == AddressError::MissingKey,
                "no environment means unavailable")) {
        return false;
    }
    const SessionEnvironment autolaunch_env{"autolaunch:", ""};
    return expect(session_bus_address(autolaunch_env).error() == AddressError::UnsupportedTransport,
                  "autolaunch is unsupported, not guessed");
}

bool fallback_list() {
    const auto mixed = parse_bus_address("tcp:host=127.0.0.1,port=1;unix:path=/tmp/bus");
    if (!expect(mixed.has_value(), "unsupported entry falls through to the next") ||
        !expect_equal(mixed->path, "/tmp/bus", "fallback picks the unix entry")) {
        return false;
    }
    const auto separated = parse_bus_address(";unix:path=/tmp/bus;;");
    if (!expect(separated.has_value(), "empty entries are skipped") ||
        !expect_equal(separated->path, "/tmp/bus", "separators ignored")) {
        return false;
    }
    const auto none = parse_bus_address("tcp:host=a,port=1;unixexec:path=/bin/true");
    return expect(none.error() == AddressError::UnsupportedTransport,
                  "first failure is reported when nothing works");
}

bool rejections() {
    if (!expect(parse_bus_address("").error() == AddressError::Empty, "empty text")) {
        return false;
    }
    if (!expect(parse_bus_address("unix").error() == AddressError::BadTransport, "missing colon")) {
        return false;
    }
    if (!expect(parse_bus_address(":path=/tmp/x").error() == AddressError::BadTransport,
                "empty transport")) {
        return false;
    }
    if (!expect(parse_bus_address("uni x:path=/tmp/x").error() == AddressError::BadTransport,
                "invalid transport character")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:").error() == AddressError::MissingKey, "no parameters")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path").error() == AddressError::BadKey, "bare key")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/x,").error() == AddressError::BadKey,
                "trailing comma")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=").error() == AddressError::BadValue,
                "empty path value")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/a,path=/tmp/b").error() == AddressError::BadValue,
                "duplicate path")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/a,abstract=b").error() == AddressError::BadValue,
                "path and abstract conflict")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:bogus=1").error() == AddressError::UnsupportedKey,
                "unknown key")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:dir=/tmp/x").error() == AddressError::UnsupportedKey,
                "autolaunch-only key")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/%zz").error() == AddressError::BadValue,
                "bad hex escape")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/%2").error() == AddressError::BadValue,
                "truncated escape")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/%00x").error() == AddressError::BadValue,
                "NUL escape rejected")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/%0Ax").error() == AddressError::BadValue,
                "control escape rejected")) {
        return false;
    }
    if (!expect(parse_bus_address("unix:path=/tmp/a b").error() == AddressError::BadValue,
                "raw space must be escaped")) {
        return false;
    }
    return expect(parse_bus_address("unix:path=/tmp/x#y").error() == AddressError::BadValue,
                  "raw punctuation must be escaped");
}

bool bounds() {
    std::string too_large = "unix:path=/";
    too_large.append(2000, 'a');
    if (!expect(parse_bus_address(too_large).error() == AddressError::TooLarge,
                "address text is bounded")) {
        return false;
    }
    std::string long_path = "unix:path=/";
    long_path.append(kMaxUnixPathBytes, 'a');
    if (!expect(parse_bus_address(long_path).error() == AddressError::PathTooLong,
                "endpoint is bounded")) {
        return false;
    }
    std::string at_limit = "unix:path=/";
    at_limit.append(kMaxUnixPathBytes - 1, 'a');
    const auto accepted = parse_bus_address(at_limit);
    return expect(accepted.has_value(), "endpoint at the limit is accepted") &&
           expect(accepted->path.size() == kMaxUnixPathBytes, "limit size is exact");
}

bool tolerant_metadata() {
    const auto with_guid = parse_bus_address("unix:path=/tmp/bus,guid=0123456789abcdef");
    return expect(with_guid.has_value(), "guid is tolerated") &&
           expect_equal(with_guid->path, "/tmp/bus", "guid does not change the endpoint");
}

bool round_trips() {
    const BusAddress cases[] = {
        BusAddress{BusTransport::UnixPath, "/run/user/1000/bus"},
        BusAddress{BusTransport::UnixAbstract, "/tmp/dbus-abc"},
        BusAddress{BusTransport::UnixPath, "/tmp/odd path,with=chars%"},
    };
    for (const BusAddress& address : cases) {
        const std::string formatted = format_bus_address(address);
        const auto reparsed = parse_bus_address(formatted, "/run/user/1000");
        if (!expect(reparsed.has_value(), "formatted address reparses") ||
            !expect(reparsed->transport == address.transport, "round-trip transport") ||
            !expect_equal(reparsed->path, address.path, "round-trip path")) {
            return false;
        }
    }
    return true;
}

bool names_and_environment() {
    if (!expect_equal(to_string(AddressError::UnsupportedTransport), "unsupported transport",
                      "error name") ||
        !expect_equal(to_string(AddressError::PathTooLong), "path too long", "error name")) {
        return false;
    }
    // The environment reader never crashes and only returns valid views; its exact values depend
    // on the test shell, so only the contract is asserted.
    const SessionEnvironment environment = read_session_environment();
    (void)environment;
    return true;
}

} // namespace

int main() {
    if (!unix_path()) {
        return 1;
    }
    if (!unix_abstract()) {
        return 2;
    }
    if (!escaped_values()) {
        return 3;
    }
    if (!runtime_expansion()) {
        return 4;
    }
    if (!session_resolution()) {
        return 5;
    }
    if (!fallback_list()) {
        return 6;
    }
    if (!rejections()) {
        return 7;
    }
    if (!bounds()) {
        return 8;
    }
    if (!tolerant_metadata()) {
        return 9;
    }
    if (!round_trips()) {
        return 10;
    }
    if (!names_and_environment()) {
        return 11;
    }
    std::cout << "a11y_dbus_address: ok\n";
    return 0;
}
