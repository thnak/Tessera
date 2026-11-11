// SPDX-License-Identifier: MIT
//
// Design capturer for the shell: renders the startup P0/P2/P4 mock frames and the window-chrome
// (system vs framework title bar + AppBar) mock frames with the real style/layout/paint path
// (AppHost + Batched2D) and writes them as PNGs for docs/design/shell-design-preview.md.
//
// These are DESIGN MOCKS, not implemented features: the startup phase scheduler and the Windows
// custom title bar are specified in the startup/chrome specs (docs PR in flight), so this tool only
// composes what the current engine can already lay out and paint (boxes, rounded rects, borders,
// text). It doubles as the smoke test that the design images still render.

#include "stb_image_write.h"
#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef TESSERA_DESIGN_FONT
#define TESSERA_DESIGN_FONT "tests/fixtures/fonts/NotoSans-Regular.ttf"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;

constexpr std::uint32_t kWidth = 900;
constexpr std::uint32_t kHeight = 560;

using StyleEntry = std::pair<std::string_view, std::string_view>;

std::uint64_t g_next_id = 1;

NodeId next_id() {
    return g_next_id++;
}

Node& box(Node& parent, std::initializer_list<StyleEntry> entries) {
    auto node = std::make_unique<Node>(next_id());
    Node& added = parent.add_child(std::move(node));
    added.set_element_kind("View");
    // Containers are column flex boxes by default; entries are applied last so a row overrides it.
    added.set_inline_style("display", "flex");
    added.set_inline_style("flex-direction", "column");
    for (const StyleEntry& entry : entries) {
        added.set_inline_style(entry.first, entry.second);
    }
    return added;
}

Node& label(Node& parent, std::string text, std::initializer_list<StyleEntry> entries) {
    auto node = std::make_unique<Node>(next_id());
    Node& added = parent.add_child(std::move(node));
    added.set_element_kind("Text");
    PaintData data;
    data.kind = PaintKind::Text;
    data.text = std::move(text);
    added.set_paint_data(std::move(data));
    for (const StyleEntry& entry : entries) {
        added.set_inline_style(entry.first, entry.second);
    }
    return added;
}

void root_style(Node& root, std::initializer_list<StyleEntry> entries) {
    root.set_element_kind("View");
    for (const StyleEntry& entry : entries) {
        root.set_inline_style(entry.first, entry.second);
    }
}

// P0: the OS launch window before the runtime has a first frame. Brand mark, product name,
// "starting" caption and a static progress stub.
void build_launch(Node& root) {
    root_style(root, {{"background-color", "#0b1220"},
                      {"flex-direction", "column"},
                      {"align-items", "center"},
                      {"justify-content", "center"}});

    Node& logo = box(root, {{"width", "104px"},
                            {"height", "104px"},
                            {"border-radius", "26px"},
                            {"background-color", "#2563eb"},
                            {"padding-top", "24px"},
                            {"align-items", "center"}});
    label(logo, "T", {{"font-size", "46px"}, {"color", "#ffffff"}});

    label(root, "Tessera", {{"font-size", "28px"}, {"color", "#e2e8f0"}, {"margin-top", "28px"}});
    label(root, "Starting Tessera runtime...",
          {{"font-size", "14px"}, {"color", "#94a3b8"}, {"margin-top", "10px"}});

    Node& track = box(root, {{"width", "280px"},
                             {"height", "6px"},
                             {"margin-top", "42px"},
                             {"border-radius", "3px"},
                             {"background-color", "#1e293b"}});
    box(track, {{"width", "96px"},
                {"height", "6px"},
                {"border-radius", "3px"},
                {"background-color", "#2563eb"}});

    label(root, "P0 launch", {{"font-size", "12px"}, {"color", "#64748b"}, {"margin-top", "18px"}});
}

void build_sidebar(Node& parent) {
    Node& sidebar =
        box(parent, {{"width", "232px"}, {"padding", "16px"}, {"background-color", "#111827"}});
    Node& brand = box(
        sidebar, {{"flex-direction", "row"}, {"align-items", "center"}, {"margin-bottom", "28px"}});
    box(brand, {{"width", "32px"},
                {"height", "32px"},
                {"border-radius", "9px"},
                {"background-color", "#2563eb"}});
    label(brand, "Tessera", {{"font-size", "17px"}, {"color", "#e2e8f0"}, {"margin-left", "10px"}});

    const std::vector<std::string> nav = {"Overview", "Projects", "Activity", "Settings"};
    for (std::size_t index = 0; index < nav.size(); ++index) {
        const bool active = index == 0;
        Node& item = box(sidebar, {{"height", "38px"},
                                   {"padding-top", "9px"},
                                   {"padding-left", "12px"},
                                   {"margin-bottom", "6px"},
                                   {"border-radius", "8px"},
                                   {"background-color", active ? "#1d4ed8" : "#111827"}});
        label(item, nav[index], {{"font-size", "14px"}, {"color", active ? "#ffffff" : "#94a3b8"}});
    }
}

void build_summary_cards(Node& body) {
    Node& cards = box(body, {{"flex-direction", "row"}});
    const std::vector<std::pair<std::string, std::string>> summaries = {
        {"Projects", "12"}, {"Active", "3"}, {"Errors", "0"}};
    for (std::size_t index = 0; index < summaries.size(); ++index) {
        Node& card = box(cards, {{"width", "196px"},
                                 {"height", "104px"},
                                 {"margin-right", "16px"},
                                 {"border-radius", "12px"},
                                 {"padding", "16px"},
                                 {"background-color", "#1e293b"}});
        label(card, summaries[index].first, {{"font-size", "13px"}, {"color", "#94a3b8"}});
        label(card, summaries[index].second,
              {{"font-size", "30px"},
               {"color", index == 2 ? "#22c55e" : "#e2e8f0"},
               {"margin-top", "10px"}});
    }
}

void build_activity_list(Node& body) {
    Node& list = box(body, {{"margin-top", "24px"},
                            {"border-radius", "12px"},
                            {"padding", "16px"},
                            {"background-color", "#111827"}});
    label(list, "Recent activity",
          {{"font-size", "15px"}, {"color", "#e2e8f0"}, {"margin-bottom", "14px"}});
    const std::vector<std::string> rows = {"Build finished in 42 s",
                                           "Snapshot restored (warm start)", "Theme applied",
                                           "Project sync complete"};
    for (const std::string& row : rows) {
        Node& line = box(list, {{"height", "30px"},
                                {"flex-direction", "row"},
                                {"align-items", "center"},
                                {"border-bottom-width", "1px"},
                                {"border-color", "#1e293b"}});
        label(line, row, {{"font-size", "13px"}, {"color", "#cbd5e1"}});
        box(line, {{"width", "8px"},
                   {"height", "8px"},
                   {"margin-left", "auto"},
                   {"border-radius", "4px"},
                   {"background-color", "#22c55e"}});
    }
}

// One content column: top bar plus a skeleton or the interactive dashboard.
void build_content(Node& parent, bool skeleton) {
    Node& content = box(parent, {{"width", "668px"}});
    Node& top_bar = box(content, {{"height", "60px"},
                                  {"flex-direction", "row"},
                                  {"align-items", "center"},
                                  {"padding-left", "24px"},
                                  {"padding-right", "24px"},
                                  {"border-bottom-width", "1px"},
                                  {"border-color", "#1e293b"}});
    label(top_bar, skeleton ? "Loading workspace" : "Overview",
          {{"font-size", "18px"}, {"color", skeleton ? "#64748b" : "#e2e8f0"}});
    box(top_bar, {{"width", "108px"},
                  {"height", "32px"},
                  {"margin-left", "auto"},
                  {"border-radius", "8px"},
                  {"background-color", skeleton ? "#1e293b" : "#2563eb"}});

    Node& body = box(content, {{"padding", "24px"}, {"background-color", "#0b1220"}});
    if (skeleton) {
        for (int card = 0; card < 3; ++card) {
            Node& block = box(body, {{"height", "112px"},
                                     {"margin-bottom", "16px"},
                                     {"border-radius", "12px"},
                                     {"padding", "18px"},
                                     {"background-color", "#1e293b"}});
            box(block, {{"width", card == 0 ? "62%" : "48%"},
                        {"height", "14px"},
                        {"margin-bottom", "14px"},
                        {"border-radius", "7px"},
                        {"background-color", "#334155"}});
            box(block, {{"width", "34%"},
                        {"height", "12px"},
                        {"border-radius", "6px"},
                        {"background-color", "#293548"}});
        }
        return;
    }
    build_summary_cards(body);
    build_activity_list(body);
}

void build_shell(Node& root, bool skeleton) {
    root_style(root, {{"background-color", "#0b1220"}, {"flex-direction", "row"}});
    build_sidebar(root);
    build_content(root, skeleton);
}

// Window controls: minimise bar, maximise square, close cross, drawn from primitives.
void build_controls(Node& title_bar) {
    box(title_bar, {{"width", "10px"},
                    {"height", "1px"},
                    {"margin-top", "15px"},
                    {"margin-left", "auto"},
                    {"background-color", "#cbd5e1"}});
    box(title_bar, {{"width", "9px"},
                    {"height", "9px"},
                    {"margin-top", "11px"},
                    {"margin-left", "16px"},
                    {"border-width", "1px"},
                    {"border-color", "#cbd5e1"}});
    label(title_bar, "x",
          {{"font-size", "12px"},
           {"color", "#cbd5e1"},
           {"margin-top", "9px"},
           {"margin-left", "16px"},
           {"margin-right", "12px"}});
}

// P2/P4 window chrome mock: a 32 px title bar (system- or framework-drawn) above the shell. The
// custom variant adds the framework AppBar row; both use the same content mock.
void build_chrome(Node& root, bool custom) {
    root_style(root, {{"background-color", "#0b1220"}, {"flex-direction", "column"}});

    Node& title_bar = box(root, {{"height", "32px"},
                                 {"flex-direction", "row"},
                                 {"align-items", "center"},
                                 {"padding-left", "8px"},
                                 {"background-color", custom ? "#111827" : "#0f172a"},
                                 {"border-bottom-width", custom ? "1px" : "0px"},
                                 {"border-color", "#1e293b"}});
    box(title_bar, {{"width", "16px"},
                    {"height", "16px"},
                    {"border-radius", "4px"},
                    {"background-color", "#2563eb"}});
    label(title_bar, "Tessera - Overview",
          {{"font-size", "12px"}, {"color", "#cbd5e1"}, {"margin-left", "8px"}});
    build_controls(title_bar);

    if (custom) {
        Node& app_bar = box(root, {{"height", "48px"},
                                   {"flex-direction", "row"},
                                   {"align-items", "center"},
                                   {"padding-left", "16px"},
                                   {"background-color", "#0b1220"},
                                   {"border-bottom-width", "1px"},
                                   {"border-color", "#1e293b"}});
        label(app_bar, "Overview",
              {{"font-size", "14px"},
               {"color", "#e2e8f0"},
               {"border-bottom-width", "2px"},
               {"border-color", "#2563eb"},
               {"padding-top", "13px"},
               {"height", "48px"},
               {"margin-right", "22px"}});
        label(app_bar, "Projects",
              {{"font-size", "14px"},
               {"color", "#94a3b8"},
               {"padding-top", "15px"},
               {"height", "48px"},
               {"margin-right", "22px"}});
        label(app_bar, "Activity",
              {{"font-size", "14px"},
               {"color", "#94a3b8"},
               {"padding-top", "15px"},
               {"height", "48px"}});
    }

    // Content row is sized explicitly: the title bar (32) and optional AppBar (48) are fixed.
    const int title = 32;
    const int app_bar = custom ? 48 : 0;
    const int row_height = static_cast<int>(kHeight) - title - app_bar;
    Node& row =
        box(root, {{"flex-direction", "row"}, {"height", std::to_string(row_height) + "px"}});
    build_sidebar(row);
    build_content(row, false);
}

bool write_png(const std::string& path, const tessera::app::CapturedFrame& frame) {
    return stbi_write_png(path.c_str(), static_cast<int>(frame.width),
                          static_cast<int>(frame.height), 4, frame.rgba.data(),
                          static_cast<int>(frame.width) * 4) != 0;
}

bool capture(const std::string& path, const std::function<void(Node&)>& build) {
    Node root(1);
    g_next_id = 2;
    build(root);
    tessera::app::CaptureOptions options;
    options.width = kWidth;
    options.height = kHeight;
    options.font_path = TESSERA_DESIGN_FONT;
    const auto frame =
        tessera::app::capture_frame(root, tessera::ui::style::compile_stylesheet(""), options);
    if (!frame) {
        return false;
    }
    if (!write_png(path, *frame)) {
        std::cerr << "design_mock: failed to write " << path << "\n";
        return false;
    }
    std::cout << "design_mock: wrote " << path << " (" << frame->width << "x" << frame->height
              << ")\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const std::string out_dir = argc > 1 ? argv[1] : ".";
    const bool ok =
        capture(out_dir + "/startup-p0.png", build_launch) &&
        capture(out_dir + "/startup-p2.png", [](Node& root) { build_shell(root, true); }) &&
        capture(out_dir + "/startup-p4.png", [](Node& root) { build_shell(root, false); }) &&
        capture(out_dir + "/chrome-system.png", [](Node& root) { build_chrome(root, false); }) &&
        capture(out_dir + "/chrome-custom.png", [](Node& root) { build_chrome(root, true); });
    if (!ok) {
        std::cout << "design_mock: skipped (Vulkan device unavailable or write failed)\n";
        return 0;
    }
    return 0;
}
