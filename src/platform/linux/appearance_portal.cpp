// SPDX-License-Identifier: MIT
//
// The Linux appearance source: the freedesktop settings portal, with the GTK settings file as the
// documented fallback (issue #187). See appearance_portal.hpp for the threading contract and
// include/tessera/hal/portal_appearance.hpp for the pure decode this file drives.
//
// The byte plumbing intentionally mirrors `AtspiSession` (src/ui/accessibility/
// atspi_session_handshake.cpp): one transport, SASL EXTERNAL, `Hello`, then request/reply
// correlated by serial. It is smaller because the portal surface is one interface with two `Read`
// calls and one signal, not a whole accessibility tree.

#include "appearance_portal.hpp"
#include "tessera/dbus/address.hpp"
#include "tessera/dbus/auth.hpp"
#include "tessera/dbus/wire.hpp"
#include "tessera/hal/portal_appearance.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <span>
#include <string_view>
#include <vector>

namespace tessera::hal::detail {
namespace {

namespace dbus = tessera::dbus;

// The settings portal's well-known name, object path and interface.
constexpr std::string_view kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr std::string_view kPortalPath = "/org/freedesktop/portal/desktop";
constexpr std::string_view kPortalInterface = "org.freedesktop.portal.Settings";
constexpr std::string_view kReadMember = "Read";
constexpr std::string_view kSettingChangedMember = "SettingChanged";
constexpr std::string_view kAddMatchMember = "AddMatch";

// The session bus handshake (the same names `AtspiSession` uses).
constexpr std::string_view kSessionBusName = "org.freedesktop.DBus";
constexpr std::string_view kSessionBusPath = "/org/freedesktop/DBus";
constexpr std::string_view kSessionBusInterface = "org.freedesktop.DBus";
constexpr std::string_view kHelloMember = "Hello";

// The one match rule the watcher installs: the settings portal's own change signal. A broadcast for
// any other interface is filtered by the bus, so the worker is not woken by unrelated traffic.
constexpr std::string_view kMatchRule =
    "type='signal',interface='org.freedesktop.portal.Settings',member='SettingChanged'";

// Bounded work per pump: a wedged or hostile peer can never make the worker spin or allocate
// without bound.
constexpr int kReadTimeoutMs = 50;
constexpr std::size_t kReadChunkBytes = 4096;
constexpr std::size_t kMaxMessagesPerPump = 32;
constexpr std::size_t kMaxFrameBytes = 256 * 1024;
constexpr std::size_t kMaxGtkSettingsBytes = 64 * 1024;

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

// The number of bytes one complete message occupies, or 0 when more are needed. The D-Bus fixed
// header is 16 bytes: endian flag, type, flags, version, body length, serial, header-fields length;
// the body starts at the first 8-byte boundary after the header fields.
std::size_t frame_size(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16) {
        return 0;
    }
    bool big_endian = false;
    if (bytes[0] == 'l') {
        big_endian = false;
    } else if (bytes[0] == 'B') {
        big_endian = true;
    } else {
        return 0;
    }
    const std::size_t body_length = read_u32(bytes.data() + 4, big_endian);
    const std::size_t fields_length = read_u32(bytes.data() + 12, big_endian);
    if (body_length > kMaxFrameBytes || fields_length > kMaxFrameBytes) {
        return 0;
    }
    const std::size_t header = 16 + fields_length;
    const std::size_t padded = (header + 7) & ~std::size_t{7};
    const std::size_t total = padded + body_length;
    return total <= bytes.size() ? total : 0;
}

// Reports one unavailable path, once, so a machine with no portal produces one line rather than a
// stream (ADR-022: bounded, privacy-safe diagnostics).
void report_once(bool& reported, const char* message) {
    if (reported) {
        return;
    }
    reported = true;
    std::fprintf(stderr, "TESSERA_APPEARANCE_UNAVAILABLE: %s\n", message);
}

} // namespace

const char* appearance_source_name(AppearanceSource source) noexcept {
    switch (source) {
    case AppearanceSource::None:
        return "none";
    case AppearanceSource::Portal:
        return "portal";
    case AppearanceSource::GtkSettings:
        return "gtk-settings";
    }
    return "unknown";
}

std::string gtk_settings_path() {
    // GTK reads $XDG_CONFIG_HOME/gtk-3.0/settings.ini, falling back to ~/.config. This is the one
    // environment read in the feature; start() does it, never the frame path.
    auto read_env = [](const char* name) -> std::string {
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
    std::string base = read_env("XDG_CONFIG_HOME");
    if (base.empty()) {
        const std::string home = read_env("HOME");
        if (home.empty()) {
            return {};
        }
        base = home + "/.config";
    }
    return base + "/gtk-3.0/settings.ini";
}

std::unique_ptr<dbus::Transport> UnixPortalTransportProvider::create() {
    return dbus::create_unix_socket_transport();
}

AppearancePortal::AppearancePortal(PortalTransportProvider* provider)
    : provider_(provider),
      owned_provider_(provider == nullptr ? std::make_unique<UnixPortalTransportProvider>()
                                          : nullptr),
      snapshot_bits_(appearance_to_bits(assumed_appearance())) {
    if (provider_ == nullptr) {
        provider_ = owned_provider_.get();
    }
}

AppearancePortal::~AppearancePortal() {
    stop();
}

void AppearancePortal::start() {
    if (thread_.joinable()) {
        return;
    }
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::exception&) {
        // No thread: the fallback stands, and one diagnostic says why. A missing preference must
        // never take the app down.
        std::fprintf(stderr,
                     "TESSERA_APPEARANCE_UNAVAILABLE: the watcher thread could not start\n");
    }
}

void AppearancePortal::stop() {
    stop_requested_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AppearancePortal::publish(AppearancePreference appearance, AppearanceSource source) {
    const std::uint32_t bits = appearance_to_bits(appearance);
    const std::uint32_t previous = snapshot_bits_.exchange(bits, std::memory_order_acq_rel);
    source_.store(source, std::memory_order_release);
    measured_.store(true, std::memory_order_release);
    if (previous != bits) {
        // Only a real move is an event. The consumer drains one event and reads the newest value,
        // so intermediate values are coalesced by construction (the `MotionPreference` rule).
        change_pending_.store(true, std::memory_order_release);
    }
}

void AppearancePortal::run() {
    bool reported = false;
    if (connect_and_subscribe()) {
        connected_.store(true, std::memory_order_release);
    } else if (!attempt_gtk_settings()) {
        report_once(reported, "no session bus, portal or GTK settings; keeping the defaults");
    }
    while (!stop_requested_.load(std::memory_order_acquire)) {
        (void)pump(kReadTimeoutMs);
    }
    if (transport_ != nullptr) {
        transport_->close();
        transport_.reset();
    }
}

bool AppearancePortal::flush_output() {
    if (transport_ == nullptr) {
        return false;
    }
    if (out_.empty()) {
        return true;
    }
    const auto written = transport_->write_all(out_);
    out_.clear();
    return written.has_value();
}

bool AppearancePortal::connect_and_subscribe() {
    const dbus::SessionEnvironment environment = dbus::read_session_environment();
    const auto address = dbus::session_bus_address(environment);
    if (!address) {
        return false;
    }
    transport_ = provider_ != nullptr ? provider_->create() : nullptr;
    if (transport_ == nullptr) {
        return false;
    }
    if (!transport_->connect(*address)) {
        transport_.reset();
        return false;
    }
    auth_ = std::make_unique<dbus::AuthClient>(dbus::external_identity(dbus::current_uid()));
    const auto request = auth_->initial_request();
    if (!request) {
        transport_.reset();
        return false;
    }
    out_.assign(request->begin(), request->end());
    if (!flush_output()) {
        transport_.reset();
        return false;
    }
    return true;
}

bool AppearancePortal::attempt_gtk_settings() {
    const std::string path = gtk_settings_path();
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > kMaxGtkSettingsBytes) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string text(size, '\0');
    if (!file.read(text.data(), static_cast<std::streamsize>(size))) {
        return false;
    }
    const GtkSettingsAnswer answer = parse_gtk_settings(text);
    if (!answer.saw_animations && !answer.saw_dark_theme) {
        return false;
    }
    publish(answer.appearance, AppearanceSource::GtkSettings);
    return true;
}

std::size_t AppearancePortal::pump(int timeout_ms) {
    if (transport_ == nullptr || !transport_->connected()) {
        return 0;
    }
    // Authentication first: every byte up to BEGIN goes through the SASL state machine, and its own
    // buffered line handling means a partial line is not lost between reads.
    if (auth_ != nullptr) {
        std::array<std::uint8_t, kReadChunkBytes> buffer{};
        const auto read = transport_->read_some(buffer, timeout_ms);
        if (!read) {
            if (read.error() == dbus::TransportError::Closed) {
                transport_->close();
            }
            return 0;
        }
        const auto phase =
            auth_->consume(std::span<const std::uint8_t>(buffer.data(), *read), out_);
        if (!phase) {
            auth_.reset();
            return 0;
        }
        if (*phase != dbus::AuthPhase::Complete) {
            (void)flush_output();
            return 0;
        }
        auth_.reset();
        // BEGIN is already in `out_`: the binary protocol starts on this connection, with `Hello`.
        if (!send_hello()) {
            return 0;
        }
        return 0;
    }

    // Binary phase: one bounded read, then every complete message it completes.
    std::array<std::uint8_t, kReadChunkBytes> buffer{};
    const auto read = transport_->read_some(buffer, timeout_ms);
    if (!read) {
        if (read.error() == dbus::TransportError::Closed) {
            transport_->close();
        }
        return 0;
    }
    if (*read == 0) {
        return 0;
    }
    if (in_.size() + *read > kMaxFrameBytes) {
        // A peer that never completes a frame is not a usable portal: drop the connection rather
        // than growing the buffer without bound.
        transport_->close();
        return 0;
    }
    in_.insert(in_.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*read));

    std::size_t changes = 0;
    std::size_t messages = 0;
    std::size_t offset = 0;
    while (messages < kMaxMessagesPerPump) {
        const std::size_t size = frame_size(std::span<const std::uint8_t>(in_).subspan(offset));
        if (size == 0) {
            break;
        }
        if (handle_message(std::span<const std::uint8_t>(in_).subspan(offset, size))) {
            ++changes;
        }
        offset += size;
        ++messages;
    }
    if (offset > 0) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    (void)flush_output();
    return changes;
}

bool AppearancePortal::send_hello() {
    dbus::MessageHeader header;
    header.type = dbus::MessageType::MethodCall;
    header.serial = next_serial();
    pending_hello_serial_ = header.serial;
    header.destination = std::string(kSessionBusName);
    header.path = std::string(kSessionBusPath);
    header.interface = std::string(kSessionBusInterface);
    header.member = std::string(kHelloMember);
    dbus::MessageWriter writer;
    if (!writer.begin_message(header) || !writer.end_message()) {
        return false;
    }
    out_.insert(out_.end(), writer.bytes().begin(), writer.bytes().end());
    return flush_output();
}

// Subscribes to the portal's `SettingChanged` on the session bus. `AddMatch` lives on
// `org.freedesktop.DBus`, not on the portal, so it is a separate destination from the reads.
bool AppearancePortal::send_add_match() {
    if (fired_match_rule_) {
        return true;
    }
    fired_match_rule_ = true;
    dbus::MessageHeader header;
    header.type = dbus::MessageType::MethodCall;
    header.serial = next_serial();
    header.destination = std::string(kSessionBusName);
    header.path = std::string(kSessionBusPath);
    header.interface = std::string(kSessionBusInterface);
    header.member = std::string(kAddMatchMember);
    header.signature = "s";
    dbus::MessageWriter writer;
    if (!writer.begin_message(header)) {
        return false;
    }
    writer.write_string(kMatchRule);
    if (!writer.end_message()) {
        return false;
    }
    out_.insert(out_.end(), writer.bytes().begin(), writer.bytes().end());
    return flush_output();
}

bool AppearancePortal::request_setting(std::string_view key) {
    // `Read` takes (ss): the namespace and the key. The serial is recorded so the reply is matched
    // to its own key, never to whichever answer arrives first.
    dbus::MessageHeader header;
    header.type = dbus::MessageType::MethodCall;
    header.serial = next_serial();
    if (key == kPortalColorSchemeKey) {
        pending_color_scheme_serial_ = header.serial;
    } else if (key == kPortalEnableAnimationsKey) {
        pending_animations_serial_ = header.serial;
    }
    header.destination = std::string(kPortalBusName);
    header.path = std::string(kPortalPath);
    header.interface = std::string(kPortalInterface);
    header.member = std::string(kReadMember);
    header.signature = "ss";
    dbus::MessageWriter writer;
    if (!writer.begin_message(header)) {
        return false;
    }
    writer.write_string(kPortalAppearanceNamespace);
    writer.write_string(key);
    if (!writer.end_message()) {
        return false;
    }
    out_.insert(out_.end(), writer.bytes().begin(), writer.bytes().end());
    return flush_output();
}

bool AppearancePortal::handle_message(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 16) {
        return false;
    }
    const bool big_endian = bytes[0] == 'B';
    const dbus::MessageType type = static_cast<dbus::MessageType>(bytes[1]);
    const std::uint32_t reply_serial = read_u32(bytes.data() + 8, big_endian);
    const auto parsed = dbus::parse_message(bytes);
    if (!parsed) {
        return false;
    }
    if (type == dbus::MessageType::MethodReturn) {
        // A handshake reply: `Hello` answered means the connection is usable, so the match rule and
        // the two initial reads go out now (one round trip for the whole subscription).
        if (reply_serial == pending_hello_serial_ && !fired_initial_reads_) {
            fired_initial_reads_ = true;
            if (!send_add_match() || !request_setting(kPortalColorSchemeKey) ||
                !request_setting(kPortalEnableAnimationsKey)) {
                transport_->close();
            }
            return false;
        }
        // A reply to one of our two `Read` calls: match it against the serial we sent. The key is
        // recovered from the serial, so a reply can never be applied to the wrong preference.
        if (reply_serial == pending_color_scheme_serial_) {
            const auto update =
                decode_portal_read_reply(kPortalColorSchemeKey, parsed->body, big_endian);
            if (update && update->recognized) {
                publish(with_color_scheme(snapshot(), update->color_scheme),
                        AppearanceSource::Portal);
                return true;
            }
            return false;
        }
        if (reply_serial == pending_animations_serial_) {
            const auto update =
                decode_portal_read_reply(kPortalEnableAnimationsKey, parsed->body, big_endian);
            if (update && update->recognized) {
                publish(with_reduced_motion(snapshot(), update->reduced_motion),
                        AppearanceSource::Portal);
                return true;
            }
            return false;
        }
        return false;
    }
    if (type != dbus::MessageType::Signal) {
        return false;
    }
    const dbus::MessageHeader& header = parsed->header;
    if (header.interface != kPortalInterface || header.member != kSettingChangedMember) {
        return false;
    }
    const auto update = decode_portal_setting_changed(parsed->body, big_endian);
    if (!update || !update->recognized) {
        return false;
    }
    if (update->is_color_scheme) {
        publish(with_color_scheme(snapshot(), update->color_scheme), AppearanceSource::Portal);
    } else {
        publish(with_reduced_motion(snapshot(), update->reduced_motion), AppearanceSource::Portal);
    }
    return true;
}

} // namespace tessera::hal::detail
