// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/dbus_address.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace tessera::ui::a11y {
namespace {

// Bytes that may appear unescaped in an address value. The D-Bus grammar allows the ASCII
// alphanumerics plus a small punctuation set; the list separators are excluded by construction.
bool is_unreserved(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '/' ||
           value == '.' || value == '\\';
}

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

bool is_transport_char(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_';
}

bool is_key_char(char value) {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_';
}

std::expected<std::string, AddressError> decode_value(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t index = 0; index < raw.size(); ++index) {
        const char value = raw[index];
        if (value != '%') {
            if (!is_unreserved(value)) {
                return std::unexpected(AddressError::BadValue);
            }
            out.push_back(value);
            continue;
        }
        if (index + 2 >= raw.size()) {
            return std::unexpected(AddressError::BadValue);
        }
        const int high = hex_value(raw[index + 1]);
        const int low = hex_value(raw[index + 2]);
        if (high < 0 || low < 0) {
            return std::unexpected(AddressError::BadValue);
        }
        index += 2;
        const auto decoded = static_cast<unsigned int>((high << 4) | low);
        // Control bytes and NUL cannot occur in a useful socket path, and accepting them would let
        // an address smuggle a truncated name past the socket layer.
        if (decoded < 0x20U || decoded == 0x7FU) {
            return std::unexpected(AddressError::BadValue);
        }
        out.push_back(static_cast<char>(decoded));
    }
    return out;
}

struct SegmentState {
    std::optional<std::string> path;
    std::optional<std::string> abstract_name;
    bool runtime = false;
};

std::expected<void, AddressError> apply_parameter(SegmentState& state, std::string_view parameter) {
    const std::size_t equals = parameter.find('=');
    if (equals == std::string_view::npos || equals == 0) {
        return std::unexpected(AddressError::BadKey);
    }
    const std::string_view key = parameter.substr(0, equals);
    for (const char character : key) {
        if (!is_key_char(character)) {
            return std::unexpected(AddressError::BadKey);
        }
    }
    const std::string_view raw = parameter.substr(equals + 1);
    const auto value = decode_value(raw);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (key == "path") {
        if (state.path || state.abstract_name || state.runtime) {
            return std::unexpected(AddressError::BadValue);
        }
        if (value->empty()) {
            return std::unexpected(AddressError::BadValue);
        }
        state.path = *value;
        return {};
    }
    if (key == "abstract") {
        if (state.path || state.abstract_name || state.runtime) {
            return std::unexpected(AddressError::BadValue);
        }
        if (value->empty()) {
            return std::unexpected(AddressError::BadValue);
        }
        state.abstract_name = *value;
        return {};
    }
    if (key == "runtime") {
        if (state.path || state.abstract_name || state.runtime || *value != "yes") {
            return std::unexpected(AddressError::BadValue);
        }
        state.runtime = true;
        return {};
    }
    // `guid` carries no endpoint information and is legal in a server address; `dir`/`tmpdir` only
    // make sense for the autolaunch transport we do not implement.
    if (key == "guid") {
        return {};
    }
    return std::unexpected(AddressError::UnsupportedKey);
}

std::expected<BusAddress, AddressError> parse_segment(std::string_view segment,
                                                      std::string_view runtime_dir) {
    const std::size_t colon = segment.find(':');
    if (colon == std::string_view::npos) {
        return std::unexpected(AddressError::BadTransport);
    }
    const std::string_view transport = segment.substr(0, colon);
    if (transport.empty()) {
        return std::unexpected(AddressError::BadTransport);
    }
    for (const char character : transport) {
        if (!is_transport_char(character)) {
            return std::unexpected(AddressError::BadTransport);
        }
    }
    if (transport != "unix") {
        return std::unexpected(AddressError::UnsupportedTransport);
    }
    const std::string_view parameters = segment.substr(colon + 1);
    if (parameters.empty()) {
        return std::unexpected(AddressError::MissingKey);
    }

    SegmentState state;
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const std::size_t comma = parameters.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? parameters.size() : comma;
        if (end == start) {
            return std::unexpected(AddressError::BadKey);
        }
        if (const auto applied = apply_parameter(state, parameters.substr(start, end - start));
            !applied) {
            return std::unexpected(applied.error());
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }

    if (state.runtime) {
        if (runtime_dir.empty()) {
            return std::unexpected(AddressError::MissingKey);
        }
        BusAddress address{BusTransport::UnixPath, std::string(runtime_dir)};
        if (address.path.empty() || address.path.back() != '/') {
            address.path.push_back('/');
        }
        address.path.append("bus");
        if (address.path.size() > kMaxUnixPathBytes) {
            return std::unexpected(AddressError::PathTooLong);
        }
        return address;
    }
    if (state.path) {
        if (state.path->size() > kMaxUnixPathBytes) {
            return std::unexpected(AddressError::PathTooLong);
        }
        return BusAddress{BusTransport::UnixPath, *state.path};
    }
    if (state.abstract_name) {
        if (state.abstract_name->size() > kMaxUnixPathBytes) {
            return std::unexpected(AddressError::PathTooLong);
        }
        return BusAddress{BusTransport::UnixAbstract, *state.abstract_name};
    }
    return std::unexpected(AddressError::MissingKey);
}

} // namespace

SessionEnvironment read_session_environment() {
    // MSVC deprecates std::getenv in favour of the bounds-checked _dupenv_s variant; both return an
    // owned copy here so callers never hold a pointer into the environment block.
    auto read = [](const char* name) -> std::string {
#if defined(_MSC_VER)
        char* buffer = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&buffer, &size, name) != 0 || buffer == nullptr) {
            return {};
        }
        std::string value(buffer);
        std::free(buffer);
        return value;
#else
        const char* const value = std::getenv(name);
        return value == nullptr ? std::string{} : std::string{value};
#endif
    };
    return SessionEnvironment{read("DBUS_SESSION_BUS_ADDRESS"), read("XDG_RUNTIME_DIR")};
}

std::expected<BusAddress, AddressError> parse_bus_address(std::string_view text,
                                                          std::string_view runtime_dir) {
    if (text.empty()) {
        return std::unexpected(AddressError::Empty);
    }
    if (text.size() > kMaxAddressBytes) {
        return std::unexpected(AddressError::TooLarge);
    }
    std::optional<AddressError> first_error;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t semicolon = text.find(';', start);
        const std::size_t end = semicolon == std::string_view::npos ? text.size() : semicolon;
        const std::string_view segment = text.substr(start, end - start);
        if (!segment.empty()) {
            const auto parsed = parse_segment(segment, runtime_dir);
            if (parsed) {
                return parsed;
            }
            if (!first_error) {
                first_error = parsed.error();
            }
        }
        if (semicolon == std::string_view::npos) {
            break;
        }
        start = semicolon + 1;
    }
    return std::unexpected(first_error.value_or(AddressError::Empty));
}

std::expected<BusAddress, AddressError> session_bus_address(const SessionEnvironment& environment) {
    if (!environment.session_bus_address.empty()) {
        return parse_bus_address(environment.session_bus_address, environment.runtime_dir);
    }
    if (environment.runtime_dir.empty()) {
        return std::unexpected(AddressError::MissingKey);
    }
    return parse_bus_address("unix:runtime=yes", environment.runtime_dir);
}

std::string format_bus_address(const BusAddress& address) {
    std::string out = address.abstract() ? "unix:abstract=" : "unix:path=";
    out.reserve(out.size() + address.path.size());
    for (const char value : address.path) {
        if (is_unreserved(value)) {
            out.push_back(value);
            continue;
        }
        constexpr char kHex[] = "0123456789ABCDEF";
        const auto byte = static_cast<unsigned int>(static_cast<unsigned char>(value));
        out.push_back('%');
        out.push_back(kHex[(byte >> 4) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

std::string_view to_string(AddressError error) {
    switch (error) {
    case AddressError::None:
        return "none";
    case AddressError::Empty:
        return "empty";
    case AddressError::TooLarge:
        return "too large";
    case AddressError::BadTransport:
        return "bad transport";
    case AddressError::UnsupportedTransport:
        return "unsupported transport";
    case AddressError::BadKey:
        return "bad key";
    case AddressError::UnsupportedKey:
        return "unsupported key";
    case AddressError::BadValue:
        return "bad value";
    case AddressError::MissingKey:
        return "missing key";
    case AddressError::PathTooLong:
        return "path too long";
    }
    return "unknown";
}

} // namespace tessera::ui::a11y
