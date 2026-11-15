// SPDX-License-Identifier: MIT
//
// The Linux appearance source: the freedesktop settings portal, with the GTK settings file as the
// documented fallback (issue #187, docs/specs/os-events-v1.md section 5, ADR-020, ADR-023).
//
// Why a thread and not a callback: the portal lives behind D-Bus, and ADR-021 forbids blocking I/O
// on the UI thread. So one bounded worker thread connects the session bus (reusing the AT-SPI2
// bridge's address/SASL/transport client from `src/dbus`), calls
// `org.freedesktop.portal.Settings.Read` for `org.freedesktop.appearance`/{color-scheme,
// enable-animations}, subscribes to `SettingChanged`, and publishes each new snapshot into one
// atomic word.
//
// The UI thread only ever does an atomic load (`snapshot()`); a change reaches the app as the
// normalized `ThemeChanged`/`SystemSettingsChanged` window event the platform window enqueues when
// `take_change()`'s compare-and-clear reports the word moved. No lock, no socket and no reply wait
// on the frame path.
//
// The transport is injectable exactly like `AtspiSession`'s, so the whole handshake + read + signal
// path is testable against a deterministic loopback peer with no bus daemon and no socket.

#ifndef TESSERA_PLATFORM_LINUX_APPEARANCE_PORTAL_HPP
#define TESSERA_PLATFORM_LINUX_APPEARANCE_PORTAL_HPP

#include "tessera/dbus/transport.hpp"
#include "tessera/hal/appearance.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace tessera::hal::detail {

// Where the current snapshot came from, for the one diagnostic line.
enum class AppearanceSource : std::uint8_t {
    None = 0,    // no source answered: the documented fallback
    Portal,      // `org.freedesktop.portal.Settings`
    GtkSettings, // `settings.ini` (no portal, GTK's file is readable)
};

[[nodiscard]] const char* appearance_source_name(AppearanceSource source) noexcept;

// The GTK settings file this session would fall back to, or an empty string when there is no
// candidate. The one environment read in this feature, done once off the frame path.
[[nodiscard]] std::string gtk_settings_path();

// Creates the one connection the session uses. The default is the POSIX UNIX socket; a test injects
// a loopback-backed provider so no test needs a bus daemon.
class PortalTransportProvider {
public:
    virtual ~PortalTransportProvider() = default;
    [[nodiscard]] virtual std::unique_ptr<dbus::Transport> create() = 0;
};

class UnixPortalTransportProvider final : public PortalTransportProvider {
public:
    [[nodiscard]] std::unique_ptr<dbus::Transport> create() override;
};

// One portal session. Construction is cheap and never blocks; `start()` spawns the worker.
//
// Every read is bounded by the transport's poll timeout, so `stop()` signals and joins a thread
// that is already going to wake: shutdown can never hang on a dead bus.
class AppearancePortal final {
public:
    explicit AppearancePortal(PortalTransportProvider* provider = nullptr);
    ~AppearancePortal();

    AppearancePortal(const AppearancePortal&) = delete;
    AppearancePortal& operator=(const AppearancePortal&) = delete;

    void start();
    void stop();

    // One bounded iteration: reads what the peer sent, decodes every complete message, and returns
    // the number of appearance changes published. Never blocks past `timeout_ms`. Only the thread
    // that owns the session may call it; it is what the worker loop and a test both drive.
    std::size_t pump(int timeout_ms);

    // The newest normalized snapshot. Lock-free and allocation-free: safe on the frame path.
    [[nodiscard]] AppearancePreference snapshot() const noexcept {
        return appearance_from_bits(snapshot_bits_.load(std::memory_order_acquire));
    }
    // True exactly once per observed change (compare-and-clear).
    [[nodiscard]] bool take_change() noexcept {
        return change_pending_.exchange(false, std::memory_order_acq_rel);
    }
    [[nodiscard]] AppearanceSource source() const noexcept {
        return source_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool measured() const noexcept {
        return measured_.load(std::memory_order_acquire);
    }
    // True once the bus connection and subscription succeeded (the worker's first milestone).
    [[nodiscard]] bool connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

private:
    void publish(AppearancePreference appearance, AppearanceSource source);
    // Runs the handshake and sends the initial `Read` calls. False when the bus is unusable.
    bool connect_and_subscribe();
    bool send_hello();
    // Installs the one match rule for the portal's `SettingChanged` signal.
    bool send_add_match();
    // Issues `Read(namespace, key)` and remembers the serial, so the reply is matched to its key.
    bool request_setting(std::string_view key);
    bool flush_output();
    bool handle_message(std::span<const std::uint8_t> bytes);
    bool attempt_gtk_settings();
    void run();
    [[nodiscard]] std::uint32_t next_serial() noexcept {
        return ++serial_;
    }

    PortalTransportProvider* provider_ = nullptr;
    std::unique_ptr<PortalTransportProvider> owned_provider_;
    std::unique_ptr<dbus::Transport> transport_;
    std::unique_ptr<dbus::AuthClient> auth_;
    std::vector<std::uint8_t> out_;
    std::vector<std::uint8_t> in_;
    std::uint32_t serial_ = 0;
    std::uint32_t pending_hello_serial_ = 0;
    std::uint32_t pending_color_scheme_serial_ = 0;
    std::uint32_t pending_animations_serial_ = 0;
    bool fired_initial_reads_ = false;
    bool fired_match_rule_ = false;

    std::atomic<std::uint32_t> snapshot_bits_;
    std::atomic<bool> change_pending_{false};
    std::atomic<bool> measured_{false};
    std::atomic<bool> connected_{false};
    std::atomic<AppearanceSource> source_{AppearanceSource::None};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
};

} // namespace tessera::hal::detail

#endif // TESSERA_PLATFORM_LINUX_APPEARANCE_PORTAL_HPP
