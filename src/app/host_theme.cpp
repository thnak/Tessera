// SPDX-License-Identifier: MIT

// AppHost's embedded theme asset (issue #81, ADR-048; docs/specs/stylesheet-v1.md sections 9.1-9.2
// and docs/specs/compiled-stylesheet-v1.md section 11). Split out of host.cpp so each translation
// unit stays reviewable: the blob is loaded here, the active theme's `:root` rule is rebuilt
// through the ordinary cascade, and a rejected blob leaves the installed theme alone.

#include "host_impl.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::app {
namespace {

// The theme asset blob has no project path (it is one per project), so the report is just the code
// and the message; the current theme stays applied.
void report_theme_failure(const ui::style::ThemeBlobError& error) {
    std::fprintf(stderr, "TESSERA_THEME_BLOB_INVALID: %s: %s\n",
                 ui::style::theme_blob_error_code_name(error.code), error.message.c_str());
}

} // namespace

void AppHost::Impl::register_embedded_theme(std::span<const std::uint8_t> bytes) {
    // The asset JSON was parsed at build time; startup only decodes the blob. A failure is a
    // build/runtime version mismatch, so the current theme stays applied and nothing throws.
    const auto loaded = ui::style::load_theme_asset_blob(bytes);
    if (!loaded) {
        report_theme_failure(loaded.error());
        ++rejected_theme_count;
        return;
    }
    theme_asset = *loaded;
    rejected_theme_count = 0;
    active_theme_id = theme_asset.start;
    apply_style_generation(ui::Dirty::Style | ui::Dirty::Paint | ui::Dirty::Render);
}

void AppHost::register_embedded_theme(std::span<const std::uint8_t> bytes) {
    impl_->register_embedded_theme(bytes);
}

bool AppHost::set_theme(std::string_view id) {
    Impl& state = *impl_;
    if (state.theme_asset.find(id) == nullptr) {
        return false;
    }
    if (state.active_theme_id == id) {
        return true;
    }
    state.active_theme_id = std::string(id);
    state.apply_style_generation(ui::Dirty::Style | ui::Dirty::Paint | ui::Dirty::Render);
    return true;
}

std::vector<std::string> AppHost::theme_ids() const {
    const Impl& state = *impl_;
    std::vector<std::string> ids;
    ids.reserve(state.theme_asset.themes.size());
    for (const ui::style::Theme& theme : state.theme_asset.themes) {
        ids.push_back(theme.id);
    }
    return ids;
}

const std::string& AppHost::active_theme() const {
    return impl_->active_theme_id;
}

std::optional<std::string_view> AppHost::theme_token(std::string_view name) const {
    const Impl& state = *impl_;
    if (const ui::style::Theme* theme = state.theme_asset.find(state.active_theme_id);
        theme != nullptr) {
        if (const ui::style::ThemeToken* token = theme->find(name); token != nullptr) {
            return std::string_view(token->value);
        }
    }
    // The reserved pair keeps its built-in value when the active theme leaves it out, because it is
    // read outside the render tree (stylesheet-v1.md section 9.2).
    if (name == ui::style::kChromeBackgroundToken) {
        return ui::style::kChromeBackgroundValue;
    }
    if (name == ui::style::kChromeForegroundToken) {
        return ui::style::kChromeForegroundValue;
    }
    return std::nullopt;
}

std::size_t AppHost::rejected_theme_count() const {
    return impl_->rejected_theme_count;
}

} // namespace tessera::app
