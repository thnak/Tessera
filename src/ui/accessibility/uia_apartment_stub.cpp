// SPDX-License-Identifier: MIT

#include "tessera/ui/accessibility/uia_apartment.hpp"

#if defined(_WIN32)
#error "uia_apartment_stub.cpp is non-Windows only; Windows builds uia_apartment_win.cpp"
#endif

namespace tessera::ui::a11y {
namespace {

// Linux/macOS/Android/iOS have no UI Automation apartment. Reporting `UnsupportedPlatform` from
// `init` keeps the bridge and its public headers compiling and lets the host disable the feature
// with a diagnostic instead of linking a platform SDK it does not have.
class UnsupportedApartment final : public UiaApartment {
public:
    [[nodiscard]] std::expected<void, UiaError> init() override {
        return std::unexpected(UiaError::UnsupportedPlatform);
    }

    void shutdown() override {}

    [[nodiscard]] std::string_view name() const override {
        return "unsupported";
    }
};

} // namespace

std::unique_ptr<UiaApartment> create_platform_apartment() {
    return std::make_unique<UnsupportedApartment>();
}

} // namespace tessera::ui::a11y
