// SPDX-License-Identifier: MIT
//
// Numeric identity assignment for the IR (docs/specs/automation-ids-v1.md sections 2 and 3):
// determinism, dependence on the salt, the source path and the node path, explicit automation ids,
// ids that are already present, and the error cases.

#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"
#include "tessera/compiler/ir/validate.hpp"

#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <utility>

namespace {

namespace ir = tessera::compiler::ir;
namespace ids = tessera::compiler::ids;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ir_ids: " << message << "\n";
    }
    return condition;
}

ir::Node node(const char* id, ir::NodeKind kind = ir::NodeKind::View) {
    ir::Node value;
    value.id = id;
    value.kind = kind;
    return value;
}

// Counter in ui/Counter.tsx: a View with a Text, a Button, and a conditional Text that has a
// fallback; plus a second component with one node.
ir::IrModule make_module(const char* file = "ui/Counter.tsx") {
    ir::IrModule module;
    ir::Component counter;
    counter.id = "comp::Counter";
    counter.name = "Counter";
    counter.source.file = file;
    counter.root = node("node::1");
    counter.root.children.push_back(node("node::2", ir::NodeKind::Text));
    counter.root.children.push_back(node("node::3", ir::NodeKind::Button));
    ir::Node boundary = node("node::4", ir::NodeKind::ErrorBoundary);
    boundary.fallback = std::make_unique<ir::Node>(node("node::5", ir::NodeKind::Text));
    counter.root.children.push_back(std::move(boundary));
    module.components.push_back(std::move(counter));

    ir::Component other;
    other.id = "comp::Other";
    other.name = "Other";
    other.source.file = file;
    other.root = node("node::1");
    module.components.push_back(std::move(other));
    return module;
}

std::set<std::string> all_ui_ids(const ir::IrModule& module) {
    std::set<std::string> out;
    const auto walk = [&](auto&& self, const ir::Node& value) -> void {
        out.insert(value.ui_id);
        for (const ir::Node& child : value.children) {
            self(self, child);
        }
        if (value.fallback) {
            self(self, *value.fallback);
        }
    };
    for (const ir::Component& component : module.components) {
        walk(walk, component.root);
    }
    return out;
}

} // namespace

int main() {
    bool ok = true;

    // --- every component and node gets an id; automationId mirrors uiId ----------------------
    ir::IrModule module = make_module();
    ok = expect(ids::assign_identity(module, "factory").has_value(), "assignment succeeds") && ok;
    ok = expect(!module.components[0].component_id.empty() &&
                    module.components[0].component_id != module.components[1].component_id,
                "components get distinct ids") &&
         ok;
    const std::set<std::string> assigned = all_ui_ids(module);
    ok = expect(assigned.size() == 6 && assigned.count("") == 0,
                "six nodes (including the fallback and the second component) get distinct ids") &&
         ok;
    const ir::Node& button = module.components[0].root.children[1];
    ok = expect(button.automation_id == button.ui_id, "automationId equals uiId") && ok;
    bool identity_clean = true;
    for (const ir::IrDiagnostic& diagnostic : ir::validate(module)) {
        const bool identity = diagnostic.code == "TESSERA_IR_MISSING_ID" ||
                              diagnostic.code == "TESSERA_IR_DUPLICATE_ID" ||
                              diagnostic.code == "TESSERA_IR_AUTOMATION_ID";
        if (identity && diagnostic.message.find("node `node::") != std::string::npos) {
            identity_clean = false;
            std::cerr << "ir_ids: " << diagnostic.code << ": " << diagnostic.message << "\n";
        }
    }
    ok = expect(identity_clean, "validation reports no missing, duplicate or mismatched node id") &&
         ok;

    // --- deterministic, and keyed by salt, source path and node path -------------------------
    ir::IrModule again = make_module();
    ok = expect(ids::assign_identity(again, "factory").has_value() &&
                    again.components[0].root.children[1].ui_id == button.ui_id &&
                    again.components[0].component_id == module.components[0].component_id,
                "the same input gives the same ids") &&
         ok;
    ir::IrModule other_salt = make_module();
    ok = expect(ids::assign_identity(other_salt, "other").has_value() &&
                    other_salt.components[0].root.children[1].ui_id != button.ui_id,
                "another salt gives another id") &&
         ok;
    ir::IrModule other_file = make_module("ui/Other.tsx");
    ok = expect(ids::assign_identity(other_file, "factory").has_value() &&
                    other_file.components[0].root.children[1].ui_id != button.ui_id,
                "another source path gives another id") &&
         ok;
    ir::IrModule reordered = make_module();
    std::swap(reordered.components[0].root.children[0], reordered.components[0].root.children[1]);
    ok = expect(ids::assign_identity(reordered, "factory").has_value() &&
                    reordered.components[0].root.children[0].ui_id != button.ui_id,
                "the node path (position among siblings) is part of the id") &&
         ok;
    ok = expect(module.components[0].root.children[2].fallback->ui_id !=
                    module.components[0].root.children[2].ui_id,
                "a fallback has an id of its own") &&
         ok;

    // --- a literal key joins the hash --------------------------------------------------------
    ir::IrModule keyed_a = make_module();
    keyed_a.components[0].root.children[1].key = ir::make_literal("a");
    ir::IrModule keyed_b = make_module();
    keyed_b.components[0].root.children[1].key = ir::make_literal("b");
    ok = expect(ids::assign_identity(keyed_a, "factory").has_value() &&
                    ids::assign_identity(keyed_b, "factory").has_value() &&
                    keyed_a.components[0].root.children[1].ui_id !=
                        keyed_b.components[0].root.children[1].ui_id,
                "the key is part of the id") &&
         ok;

    // --- an explicit automationId replaces the computed id -----------------------------------
    ir::IrModule pinned = make_module();
    pinned.components[0].root.children[1].automation_id = "424242";
    ok = expect(ids::assign_identity(pinned, "factory").has_value() &&
                    pinned.components[0].root.children[1].ui_id == "424242",
                "an explicit automationId becomes the uiId") &&
         ok;

    // --- ids already present are kept ---------------------------------------------------------
    ir::IrModule preset = make_module();
    preset.components[0].component_id = "900";
    preset.components[0].root.children[0].ui_id = "901";
    ok = expect(ids::assign_identity(preset, "factory").has_value() &&
                    preset.components[0].component_id == "900" &&
                    preset.components[0].root.children[0].ui_id == "901" &&
                    !preset.components[0].root.children[1].ui_id.empty(),
                "present ids are kept and the rest are assigned") &&
         ok;

    // --- errors leave the module unchanged ----------------------------------------------------
    ir::IrModule empty_salt = make_module();
    ok = expect(!ids::assign_identity(empty_salt, "").has_value() &&
                    empty_salt.components[0].root.ui_id.empty(),
                "an empty salt is an error") &&
         ok;
    ir::IrModule duplicate = make_module();
    duplicate.components[0].root.children[0].automation_id = "7";
    duplicate.components[0].root.children[1].automation_id = "7";
    ok = expect(!ids::assign_identity(duplicate, "factory").has_value() &&
                    duplicate.components[0].component_id.empty(),
                "a duplicate explicit automationId is an error") &&
         ok;
    ir::IrModule zero = make_module();
    zero.components[0].root.children[0].automation_id = "0";
    ok = expect(!ids::assign_identity(zero, "factory").has_value(),
                "a zero automationId is an error") &&
         ok;
    ir::IrModule text = make_module();
    text.components[0].root.children[0].automation_id = "login";
    const auto bad = ids::assign_identity(text, "factory");
    ok = expect(!bad.has_value() && bad.error().message.find("non-numeric") != std::string::npos,
                "a non-numeric automationId is an error") &&
         ok;

    // --- the manifest lists every component and node, deterministically -----------------------
    const std::string manifest = ids::write_manifest(module, "factory");
    ok = expect(manifest == ids::write_manifest(again, "factory"),
                "the same input gives a byte-identical manifest") &&
         ok;
    ok = expect(manifest.find("\"name\": \"Save\"") == std::string::npos,
                "a node without aria-label has no name") &&
         ok;
    module.components[0].root.children[1].props["aria-label"] =
        ir::make_literal(std::string("Save"));
    const std::string named = ids::write_manifest(module, "factory");
    ok = expect(named.find("\"name\": \"Save\"") != std::string::npos,
                "a static aria-label becomes the manifest name") &&
         ok;
    module.components[0].root.children[1].props["aria-label"] = ir::make_ref("ref::label");
    ok = expect(ids::write_manifest(module, "factory").find("\"name\": \"Save\"") ==
                    std::string::npos,
                "a dynamic aria-label is omitted") &&
         ok;
    module.components[0].root.children[1].props["label"] = ir::make_literal(std::string("Go"));
    ok =
        expect(ids::write_manifest(module, "factory").find("\"name\": \"Go\"") == std::string::npos,
               "a dynamic aria-label hides the label fallback") &&
        ok;
    module.components[0].root.children[1].props.erase("aria-label");
    ok =
        expect(ids::write_manifest(module, "factory").find("\"name\": \"Go\"") != std::string::npos,
               "the literal label prop is the fallback name") &&
        ok;
    module.components[0].root.children[1].props.erase("label");
    module.components[0].root.children[1].text = ir::make_literal(std::string("Body"));
    ok = expect(ids::write_manifest(module, "factory").find("\"name\": \"Body\"") !=
                    std::string::npos,
                "a literal node text is the last fallback") &&
         ok;
    module.components[0].root.children[1].text = nullptr;
    ok = expect(manifest.find("\"manifestVersion\": 1") != std::string::npos &&
                    manifest.find("\"projectSalt\": \"factory\"") != std::string::npos,
                "the manifest names its version and salt") &&
         ok;
    ok = expect(manifest.find("\"componentId\": \"" + module.components[0].component_id + "\"") !=
                    std::string::npos,
                "the manifest lists the component id") &&
         ok;
    ok = expect(manifest.find("\"uiId\": \"" + button.ui_id + "\"") != std::string::npos &&
                    manifest.find("\"path\": \"Counter/root[0]/Button[1]\"") != std::string::npos,
                "the manifest maps the button's uiId to its path") &&
         ok;
    ok = expect(manifest.find("\"path\": \"Counter/root[0]/ErrorBoundary[2]/fallback[0]\"") !=
                    std::string::npos,
                "a fallback node is listed with its path") &&
         ok;
    std::size_t listed = 0;
    for (std::size_t at = manifest.find("\"uiId\""); at != std::string::npos;
         at = manifest.find("\"uiId\"", at + 1)) {
        ++listed;
    }
    ok = expect(listed == 6, "all six nodes are listed") && ok;
    ok = expect(manifest != ids::write_manifest(other_salt, "other"),
                "another salt gives another manifest") &&
         ok;

    if (!ok) {
        return 1;
    }
    std::cout << "ir_ids: ok\n";
    return 0;
}
