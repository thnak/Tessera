// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/automation/app_config.hpp"
#include "tessera/automation/node_annotation.hpp"
#include "tessera/ui/scene/node.hpp"

#include <string_view>

namespace tessera::automation {

// A scene built from an `automation.manifest.json` (docs/specs/headless-automation-v1.md section
// 2.1): one scene node per manifest node, carrying the manifest `uiId` as its automation id, with
// the manifest `kind` as its element kind and the tree shape taken from the structural paths.
//
// It is a structural description, not the app's compiled behaviour: Buttons are focusable,
// TextInputs accept committed EditText (the painted value is replaced), Scroll/VirtualList nodes
// scroll, and everything else is inert. `root` must outlive this object.
class ManifestScene final {
public:
    ManifestScene(ui::Node& root, const AppManifest& manifest);

    ManifestScene(const ManifestScene&) = delete;
    ManifestScene& operator=(const ManifestScene&) = delete;

    // Default styling so every kind gets a hit-testable size; the caller compiles it.
    [[nodiscard]] static std::string_view stylesheet() noexcept;
    // The default sheet followed by the project's own `ui.styles`, so author rules win ties.
    [[nodiscard]] static std::string stylesheet(const AppManifest& manifest);
};

// Path and component id of every manifest node by `uiId`, for EngineAutomationHost::Config so a
// snapshot reports the manifest's structural paths instead of the ones derived from the live tree.
[[nodiscard]] NodeAnnotations annotations_from_manifest(const AppManifest& manifest);

} // namespace tessera::automation
