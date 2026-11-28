// SPDX-License-Identifier: MIT
//
// `FakeNotificationBackend`: the deterministic in-tree back end (docs/specs/notifications-v1.md
// section 8; issue #452). It fakes the module's seam, like `FakePermissionBackend`, so a headless
// run and an app's own test share one definition of "the OS accepted it". Nothing here reads a
// clock: a test fires a due notification by id.

#pragma once

#include "tessera/appfw/notification.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::appfw {

class FakeNotificationBackend final : public NotificationBackend {
public:
    FakeNotificationBackend() = default;

    void set_capabilities(NotificationBackendCapabilities capabilities) noexcept {
        capabilities_ = capabilities;
    }
    // Makes the next `post` calls fail, to reach the platform-refused path.
    void set_refuse_posts(bool refuse) noexcept {
        refuse_posts_ = refuse;
    }

    // Moves an armed notification to the shown list, as the OS does when its time arrives.
    // False for an unknown id.
    bool fire(std::string_view id);

    [[nodiscard]] const std::vector<NotificationRequest>& armed() const noexcept {
        return armed_;
    }
    [[nodiscard]] const std::vector<NotificationRequest>& shown() const noexcept {
        return shown_;
    }
    [[nodiscard]] std::uint32_t badge() const noexcept {
        return badge_;
    }
    [[nodiscard]] std::size_t post_calls() const noexcept {
        return post_calls_;
    }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "fake";
    }
    [[nodiscard]] NotificationBackendCapabilities capabilities() const override {
        return capabilities_;
    }
    bool post(const NotificationRequest& request) override;
    bool cancel(std::string_view id) override;
    void cancel_all() override;
    bool set_badge(std::uint32_t count) override;

private:
    NotificationBackendCapabilities capabilities_;
    std::vector<NotificationRequest> armed_;
    std::vector<NotificationRequest> shown_;
    std::uint32_t badge_ = 0;
    std::size_t post_calls_ = 0;
    bool refuse_posts_ = false;
};

} // namespace tessera::appfw
