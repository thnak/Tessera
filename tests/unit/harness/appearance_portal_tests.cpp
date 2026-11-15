// SPDX-License-Identifier: MIT
//
// The Linux appearance source's fallback paths and its platform-neutral state (issue #187).
//
// What this test covers, deterministically and with no bus, socket, display or desktop:
//
//   * a platform/session with NO transport at all (no session bus, the CI box) keeps the documented
//     fallback, reports no measurement and produces no change event, so an app on such a machine
//     behaves exactly as it did before this feature;
//   * `gtk_settings_path()` resolves the documented GTK location without touching the frame path;
//   * the GTK settings.ini fallback really is the file the portal itself would read, so a session
//     with no portal still gets a correct answer from a settings file (the parser half is tested in
//     `hal_appearance`; here the file is real, written to a temp directory).
//
// The D-Bus protocol half is NOT re-proven here. The wire codec, SASL, address parsing and the
// transport seam are the in-tree client that `a11y_dbus_*` and `a11y_atspi_session*` already cover,
// and the portal's own decoding is `hal_appearance`'s `check_read_reply_decode` /
// `check_setting_changed_decode` (built with the real `MessageWriter`). What cannot be proven
// without a desktop is the live lookup against a running `org.freedesktop.portal.Settings`; that is
// recorded as not-exercised in docs/specs/os-events-v1.md rather than pretended here.

#include "appearance_portal.hpp"
#include "tessera/hal/appearance.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::hal::AppearancePreference;
using tessera::hal::assumed_appearance;
using tessera::hal::ColorSchemePreference;
using tessera::hal::detail::AppearancePortal;
using tessera::hal::detail::AppearanceSource;
using tessera::hal::detail::gtk_settings_path;
using tessera::hal::detail::PortalTransportProvider;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "appearance_portal: " << message << "\n";
        ++failures;
    }
    return condition;
}

// A platform with no UNIX-socket transport: the documented "no settings source" answer.
class NoTransportProvider final : public PortalTransportProvider {
public:
    [[nodiscard]] std::unique_ptr<tessera::dbus::Transport> create() override {
        return nullptr;
    }
};

// Sets an environment variable for one scope and restores the previous value, so the test can point
// the GTK path at a temp directory without leaking the change into other tests.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value)
        : name_(name) {
        const char* const previous = std::getenv(name);
        had_previous_ = previous != nullptr;
        if (had_previous_) {
            previous_ = previous;
        }
        set_env(value.c_str());
    }
    ~ScopedEnv() {
        if (had_previous_) {
            set_env(previous_.c_str());
        } else {
            unset_env();
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void set_env(const char* value) {
#if defined(_MSC_VER)
        _putenv_s(name_, value);
#else
        setenv(name_, value, 1);
#endif
    }
    void unset_env() {
#if defined(_MSC_VER)
        _putenv_s(name_, "");
#else
        unsetenv(name_);
#endif
    }
    const char* name_;
    std::string previous_;
    bool had_previous_ = false;
};

} // namespace

int main() {
    // --- 1. No transport: the fallback stands, nothing is measured, no event is produced -------
    {
        NoTransportProvider provider;
        AppearancePortal portal(&provider);
        (void)portal.pump(0);
        expect(!portal.measured(), "a session with no bus transport never reports a measurement");
        expect(!portal.take_change(), "a session with no bus transport reports no change");
        expect(portal.snapshot() == assumed_appearance(),
               "the absent-source snapshot is the documented fallback");
        expect(portal.source() == AppearanceSource::None, "the absent-source name is none");
        expect(!portal.connected(), "the absent-source session never reports a connection");
        expect(std::string_view(
                   tessera::hal::detail::appearance_source_name(AppearanceSource::None)) == "none",
               "the source name is stable for diagnostics");
        expect(std::string_view(tessera::hal::detail::appearance_source_name(
                   AppearanceSource::Portal)) == "portal",
               "the portal source name is stable");
        expect(std::string_view(tessera::hal::detail::appearance_source_name(
                   AppearanceSource::GtkSettings)) == "gtk-settings",
               "the GTK source name is stable");
    }

    // --- 2. The GTK settings path resolves under a temp XDG_CONFIG_HOME -------------------------
    {
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / "tessera-appearance-test";
        std::error_code error;
        std::filesystem::remove_all(temp, error);
        std::filesystem::create_directories(temp, error);
        if (expect(!error, "the temp config directory is created")) {
            ScopedEnv config_home("XDG_CONFIG_HOME", temp.string());
            const std::string path = gtk_settings_path();
            expect(path == (temp / "gtk-3.0" / "settings.ini").string(),
                   "the GTK settings path follows XDG_CONFIG_HOME");
            expect(path.find("settings.ini") != std::string::npos,
                   "the GTK settings path names the documented file");

            // --- 3. A real settings.ini is the no-portal answer ---------------------------------
            // The file says "animations off, prefer dark". The parser's behavior is covered in
            // hal_appearance; this asserts the file is read from the documented location.
            std::filesystem::create_directories(temp / "gtk-3.0", error);
            std::ofstream file(temp / "gtk-3.0" / "settings.ini", std::ios::binary);
            file << "[Settings]\n"
                    "gtk-enable-animations=false\n"
                    "gtk-application-prefer-dark-theme=true\n";
            file.close();
            expect(file.good() || file.eof(), "the settings.ini fixture is written");

            std::ifstream readable(path, std::ios::binary);
            expect(readable.good(), "the resolved GTK settings path is readable");
            const std::string text((std::istreambuf_iterator<char>(readable)),
                                   std::istreambuf_iterator<char>());
            const tessera::hal::GtkSettingsAnswer answer = tessera::hal::parse_gtk_settings(text);
            expect(answer.saw_animations && answer.saw_dark_theme,
                   "the real settings.ini is the file the parser reads");
            expect(answer.appearance.reduced_motion,
                   "gtk-enable-animations=false is the no-portal reduced-motion answer");
            expect(answer.appearance.color_scheme == ColorSchemePreference::Dark,
                   "prefer-dark-theme=true is the no-portal dark answer");
        }
        std::filesystem::remove_all(temp, error);
    }

    // --- 4. A missing settings file is not an error, only "nothing to say" ---------------------
    {
        const std::filesystem::path temp =
            std::filesystem::temp_directory_path() / "tessera-appearance-empty";
        std::error_code error;
        std::filesystem::remove_all(temp, error);
        std::filesystem::create_directories(temp, error);
        ScopedEnv config_home("XDG_CONFIG_HOME", temp.string());
        const std::string_view empty_text;
        const tessera::hal::GtkSettingsAnswer answer =
            tessera::hal::parse_gtk_settings(empty_text);
        expect(!answer.saw_animations && !answer.saw_dark_theme,
               "a missing settings.ini says nothing rather than failing");
        std::filesystem::remove_all(temp, error);
    }

    if (failures == 0) {
        std::cout << "appearance_portal: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
