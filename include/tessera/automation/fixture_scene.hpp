// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::automation {

// Automation ids of the built-in headless fixture (explicit overrides, so a script can target them
// without a manifest). They are documented in docs/specs/headless-automation-v1.md section 6.8.
inline constexpr std::uint64_t kFixtureViewId = 9001;
inline constexpr std::uint64_t kFixtureCounterId = 9002;
inline constexpr std::uint64_t kFixtureButtonId = 9003;
inline constexpr std::uint64_t kFixtureFieldId = 9004;
inline constexpr std::uint64_t kFixtureListId = 9005;
inline constexpr std::uint64_t kFixtureListItemId = 9006;
// Three realized virtual-list rows under the list. They share the template id and are told apart
// by item key (decimal text 101, 102, 103), as in automation-ids-v1 section 1.
inline constexpr std::uint64_t kFixtureRowId = 9010;
inline constexpr std::uint64_t kFixtureRowKeys[] = {101, 102, 103};

// The deterministic headless demo scene `tessera test --script` runs against until the app-config
// loader lands. `root` must outlive this object. The fixture owns the event handlers (a click on
// the button bumps the counter label; committed EditText on the field replaces its painted value),
// so a script's snapshot observes the result of a real input dispatch.
class FixtureScene final {
public:
    explicit FixtureScene(ui::Node& root);
    ~FixtureScene();

    FixtureScene(const FixtureScene&) = delete;
    FixtureScene& operator=(const FixtureScene&) = delete;

    // Stylesheet for the fixture; the caller compiles and hands it to the AppHost.
    [[nodiscard]] static std::string_view stylesheet() noexcept;

    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_;
    }
    [[nodiscard]] std::string_view typed_text() const {
        return typed_;
    }

private:
    static void on_click(void* context, const ui::Event& event);
    static void on_edit_text(void* context, const ui::Event& event);
    void refresh_label();

    ui::Node& root_;
    ui::Node* label_ = nullptr;
    ui::Node* field_ = nullptr;
    ui::Node* button_ = nullptr;
    std::uint64_t count_ = 0;
    std::string typed_;
};

} // namespace tessera::automation
