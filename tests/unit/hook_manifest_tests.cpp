// SPDX-License-Identifier: MIT
//
// The built-in hook manifest reader (docs/specs/jsx-subset-v1.md section 7.1, spike #658).
//
// The manifest is the single place that says what a library hook accepts and returns, so the reader
// has to be strict: a manifest that half-parses would let the analyzer validate against a file the
// author did not write.

#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/jsx/json.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

using tessera::compiler::jsx::HookLoweringKind;
using tessera::compiler::jsx::HookManifest;
using tessera::compiler::jsx::is_core_hook_name;
using tessera::compiler::jsx::ManifestError;
using tessera::compiler::jsx::parse_hook_manifest;

// The committed manifest is the file the build embeds, so the test reads the same bytes the front
// end will. `TESSERA_HOOKS_MANIFEST` points at it.
std::string read_manifest() {
    std::ifstream file(TESSERA_HOOKS_MANIFEST, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "hook_manifest: " << message << "\n";
    }
    return condition;
}

// The committed manifest is the one the build embeds, so it must parse and carry the V1 hooks.
bool test_committed_manifest_parses() {
    const std::string text = read_manifest();
    ManifestError error;
    const std::optional<HookManifest> manifest = parse_hook_manifest(text, &error);
    if (!expect(manifest.has_value(), "the committed manifest parses: " + error.detail)) {
        return false;
    }
    bool ok = expect(manifest->hooks.size() == 12, "the manifest declares twelve hooks");
    ok = expect(manifest->find("useTheme") != nullptr, "useTheme is declared") && ok;
    ok = expect(manifest->find("useThemeState") != nullptr, "useThemeState is declared") && ok;
    ok = expect(manifest->find("useBreakpoint") != nullptr, "useBreakpoint is declared") && ok;
    ok = expect(manifest->find("useMediaQuery") != nullptr, "useMediaQuery is declared") && ok;
    ok = expect(manifest->find("useFocusTrap") != nullptr, "useFocusTrap is declared") && ok;
    ok = expect(manifest->find("useListFocus") != nullptr, "useListFocus is declared") && ok;
    ok = expect(manifest->find("useScrollLock") != nullptr, "useScrollLock is declared") && ok;
    ok = expect(manifest->find("useHotkeys") != nullptr, "useHotkeys is declared") && ok;
    ok = expect(manifest->find("useClipboard") != nullptr, "useClipboard is declared") && ok;
    ok = expect(manifest->find("useOverflow") != nullptr, "useOverflow is declared") && ok;
    ok =
        expect(manifest->find("useInteractiveRole") != nullptr, "useInteractiveRole is declared") &&
        ok;
    ok =
        expect(manifest->find("usePressFeedback") != nullptr, "usePressFeedback is declared") && ok;
    return ok;
}

// An `environment` hook carries its provider and its writability.
bool test_environment_lowering() {
    const std::string text = read_manifest();
    const std::optional<HookManifest> manifest = parse_hook_manifest(text);
    if (!expect(manifest.has_value(), "the committed manifest parses")) {
        return false;
    }
    const auto* theme = manifest->find("useTheme");
    const auto* theme_state = manifest->find("useThemeState");
    bool ok = expect(theme != nullptr && theme->lowering == HookLoweringKind::Environment,
                     "useTheme is an environment hook");
    ok = expect(theme != nullptr && theme->provider == "theme",
                "useTheme names the theme provider") &&
         ok;
    ok = expect(theme != nullptr && !theme->writable, "useTheme is read-only") && ok;
    ok = expect(theme_state != nullptr && theme_state->writable, "useThemeState is writable") && ok;
    return ok;
}

// A `behavior` hook carries the registry name and its outputs, which is what the code generator and
// the registry test both read.
bool test_behavior_lowering() {
    const std::string text = read_manifest();
    const std::optional<HookManifest> manifest = parse_hook_manifest(text);
    if (!expect(manifest.has_value(), "the committed manifest parses")) {
        return false;
    }
    const auto* press = manifest->find("usePressFeedback");
    bool ok = expect(press != nullptr && press->lowering == HookLoweringKind::Behavior,
                     "usePressFeedback is a behavior hook");
    ok = expect(press != nullptr && press->provider == "press_feedback",
                "usePressFeedback names the press_feedback behavior") &&
         ok;
    ok = expect(press != nullptr && press->outputs.size() == 3,
                "usePressFeedback returns three outputs") &&
         ok;
    ok = expect(press != nullptr && press->params.size() == 2,
                "usePressFeedback takes a target and options") &&
         ok;
    ok = expect(press != nullptr && press->params[0].kind == "ref",
                "the first parameter is a target ref") &&
         ok;
    // `useFocusTrap` (issue #624): one target, two options, one output, and exclusive, because two
    // traps on one subtree have no defined precedence.
    const auto* trap = manifest->find("useFocusTrap");
    ok = expect(trap != nullptr && trap->lowering == HookLoweringKind::Behavior,
                "useFocusTrap is a behavior hook") &&
         ok;
    ok = expect(trap != nullptr && trap->provider == "focus_trap",
                "useFocusTrap names the focus_trap behavior") &&
         ok;
    ok = expect(trap != nullptr && trap->exclusive, "useFocusTrap is exclusive") && ok;
    ok = expect(trap != nullptr && trap->params.size() == 2,
                "useFocusTrap takes a target and options") &&
         ok;
    ok = expect(trap != nullptr && trap->outputs.size() == 1, "useFocusTrap returns one output") &&
         ok;
    // `useListFocus` (issue #625): one target, three options, one numeric output, exclusive.
    const auto* list = manifest->find("useListFocus");
    ok = expect(list != nullptr && list->lowering == HookLoweringKind::Behavior,
                "useListFocus is a behavior hook") &&
         ok;
    ok = expect(list != nullptr && list->provider == "roving_focus",
                "useListFocus names the roving_focus behavior") &&
         ok;
    ok = expect(list != nullptr && list->exclusive, "useListFocus is exclusive") && ok;
    ok =
        expect(list != nullptr && list->outputs.size() == 1 && list->outputs.front().ir == "number",
               "useListFocus returns one numeric output") &&
        ok;
    // `useMediaQuery` (issue #631): an environment hook with one string parameter and a boolean
    // return, so the analyzer's arity check is the manifest's answer rather than a name check.
    const auto* media = manifest->find("useMediaQuery");
    ok = expect(media != nullptr && media->lowering == HookLoweringKind::Environment,
                "useMediaQuery is an environment hook") &&
         ok;
    ok = expect(media != nullptr && media->provider == "media_query",
                "useMediaQuery names the media_query provider") &&
         ok;
    ok = expect(media != nullptr && media->params.size() == 1 &&
                    media->params.front().kind == "value",
                "useMediaQuery takes one value parameter") &&
         ok;
    ok = expect(media != nullptr && media->returns_ir == "bool",
                "useMediaQuery returns a boolean") &&
         ok;
    // `useScrollLock` (issue #633): one target, one option, one output, exclusive.
    const auto* lock = manifest->find("useScrollLock");
    ok = expect(lock != nullptr && lock->lowering == HookLoweringKind::Behavior,
                "useScrollLock is a behavior hook") &&
         ok;
    ok = expect(lock != nullptr && lock->provider == "scroll_lock",
                "useScrollLock names the scroll_lock behavior") &&
         ok;
    ok = expect(lock != nullptr && lock->exclusive, "useScrollLock is exclusive") && ok;
    ok = expect(lock != nullptr && lock->outputs.size() == 1, "useScrollLock returns one output") &&
         ok;
    // `useHotkeys` (issue #627): no target ref (a shortcut belongs to the window), one options
    // object, one output.
    const auto* keys = manifest->find("useHotkeys");
    ok = expect(keys != nullptr && keys->lowering == HookLoweringKind::Behavior,
                "useHotkeys is a behavior hook") &&
         ok;
    ok = expect(keys != nullptr && keys->provider == "hotkeys",
                "useHotkeys names the hotkeys behavior") &&
         ok;
    ok = expect(keys != nullptr && keys->params.size() == 1 &&
                    keys->params.front().kind == "options",
                "useHotkeys takes only an options object") &&
         ok;
    ok =
        expect(keys != nullptr && keys->outputs.size() == 1, "useHotkeys returns one output") && ok;
    // `useClipboard` (issue #632): no target ref, one options object, two outputs.
    const auto* clip = manifest->find("useClipboard");
    ok = expect(clip != nullptr && clip->lowering == HookLoweringKind::Behavior,
                "useClipboard is a behavior hook") &&
         ok;
    ok = expect(clip != nullptr && clip->provider == "clipboard_copy",
                "useClipboard names the clipboard_copy behavior") &&
         ok;
    ok = expect(clip != nullptr && clip->outputs.size() == 2, "useClipboard returns two outputs") &&
         ok;
    // `useOverflow` (issue #634): one target, two numeric options, two outputs.
    const auto* flow = manifest->find("useOverflow");
    ok = expect(flow != nullptr && flow->lowering == HookLoweringKind::Behavior,
                "useOverflow is a behavior hook") &&
         ok;
    ok = expect(flow != nullptr && flow->provider == "overflow",
                "useOverflow names the overflow behavior") &&
         ok;
    ok = expect(flow != nullptr && flow->outputs.size() == 2, "useOverflow returns two outputs") &&
         ok;
    // `useInteractiveRole` (issue #635): one target, one option, one output, exclusive.
    const auto* role = manifest->find("useInteractiveRole");
    ok = expect(role != nullptr && role->lowering == HookLoweringKind::Behavior,
                "useInteractiveRole is a behavior hook") &&
         ok;
    ok = expect(role != nullptr && role->provider == "interactive_role",
                "useInteractiveRole names the interactive_role behavior") &&
         ok;
    ok = expect(role != nullptr && role->exclusive, "useInteractiveRole is exclusive") && ok;
    return ok;
}

// A malformed document is refused rather than half-read.
bool test_malformed_json_is_refused() {
    ManifestError error;
    bool ok = expect(!parse_hook_manifest("{", &error), "an unterminated object is refused");
    ok = expect(error.code == ManifestError::Code::MalformedJson, "the error names the JSON") && ok;
    ok = expect(!parse_hook_manifest("[]", &error), "a non-object document is refused") && ok;
    return ok;
}

// The schema version is checked, not guessed at.
bool test_schema_version_is_checked() {
    ManifestError error;
    bool ok = expect(!parse_hook_manifest(R"({"hooks": []})", &error),
                     "a manifest with no schemaVersion is refused");
    ok = expect(error.code == ManifestError::Code::MissingSchemaVersion,
                "the error names the missing version") &&
         ok;
    ok = expect(!parse_hook_manifest(R"({"schemaVersion": 2, "hooks": []})", &error),
                "an unknown schemaVersion is refused") &&
         ok;
    ok = expect(error.code == ManifestError::Code::UnsupportedSchemaVersion,
                "the error names the unsupported version") &&
         ok;
    ok = expect(parse_hook_manifest(R"({"schemaVersion": 1, "hooks": []})").has_value(),
                "an empty hook list is valid") &&
         ok;
    return ok;
}

// A hook name is `use` followed by an uppercase letter, and a core hook may not be redeclared.
bool test_hook_names_are_checked() {
    ManifestError error;
    const auto with_name = [](std::string_view name) {
        return std::string(R"({"schemaVersion": 1, "hooks": [{"name": ")") + std::string(name) +
               R"(", "params": [], "returns": {"ts": "string", "ir": "string"}, )" +
               R"("lowering": {"kind": "environment", "provider": "theme"}}]})";
    };
    bool ok = expect(!parse_hook_manifest(with_name("usetheme"), &error),
                     "a lowercase hook name is refused");
    ok = expect(error.code == ManifestError::Code::InvalidName, "the error names the name") && ok;
    ok = expect(!parse_hook_manifest(with_name("useState"), &error),
                "a core hook name is refused") &&
         ok;
    ok = expect(error.code == ManifestError::Code::CoreHookName, "the error names the core hook") &&
         ok;
    ok = expect(parse_hook_manifest(with_name("useTheme")).has_value(),
                "a valid hook name is accepted") &&
         ok;
    return ok;
}

// A lowering kind this front end does not know is refused, so a newer manifest cannot be silently
// half-applied by an older compiler.
bool test_unknown_lowering_is_refused() {
    ManifestError error;
    const std::string_view text = R"({"schemaVersion": 1, "hooks": [
        {"name": "useThing", "params": [], "returns": {"ts": "string", "ir": "string"},
         "lowering": {"kind": "telepathy", "provider": "x"}}]})";
    bool ok = expect(!parse_hook_manifest(text, &error), "an unknown lowering kind is refused");
    ok = expect(error.code == ManifestError::Code::UnknownLoweringKind,
                "the error names the lowering kind") &&
         ok;
    return ok;
}

// A duplicate hook or parameter name is an authoring error, not last-wins.
bool test_duplicates_are_refused() {
    ManifestError error;
    const std::string_view duplicate_hook = R"({"schemaVersion": 1, "hooks": [
        {"name": "useTheme", "params": [], "returns": {"ts": "string", "ir": "string"},
         "lowering": {"kind": "environment", "provider": "theme"}},
        {"name": "useTheme", "params": [], "returns": {"ts": "string", "ir": "string"},
         "lowering": {"kind": "environment", "provider": "theme"}}]})";
    bool ok = expect(!parse_hook_manifest(duplicate_hook, &error), "a duplicate hook is refused");
    ok =
        expect(error.code == ManifestError::Code::DuplicateName, "the error names the duplicate") &&
        ok;

    const std::string_view duplicate_param = R"({"schemaVersion": 1, "hooks": [
        {"name": "useThing", "params": [{"name": "a", "ts": "boolean", "ir": "bool"},
                                        {"name": "a", "ts": "boolean", "ir": "bool"}],
         "returns": {"ts": "string", "ir": "string"},
         "lowering": {"kind": "environment", "provider": "x"}}]})";
    ok =
        expect(!parse_hook_manifest(duplicate_param, &error), "a duplicate parameter is refused") &&
        ok;
    ok = expect(error.code == ManifestError::Code::DuplicateParamName,
                "the error names the duplicate parameter") &&
         ok;
    return ok;
}

// The four core hooks are the ones the manifest may not redeclare.
bool test_core_hook_names() {
    bool ok = expect(is_core_hook_name("useState"), "useState is a core hook");
    ok = expect(is_core_hook_name("useRef"), "useRef is a core hook") && ok;
    ok = expect(is_core_hook_name("useEffect"), "useEffect is a core hook") && ok;
    ok = expect(is_core_hook_name("useAnimatedValue"), "useAnimatedValue is a core hook") && ok;
    ok = expect(!is_core_hook_name("useTheme"), "useTheme is not a core hook") && ok;
    return ok;
}

// The JSON reader itself: the shapes the manifest uses, and the ones it must refuse.
bool test_json_reader() {
    using tessera::compiler::jsx::json::parse;
    bool ok = expect(parse(R"({"a": 1, "b": [true, null, "x"]})").has_value(),
                     "an object with mixed members parses");
    ok = expect(!parse(R"({"a": 1, "a": 2})").has_value(), "a duplicate key is refused") && ok;
    ok = expect(!parse(R"({"a": 1} trailing)").has_value(), "trailing content is refused") && ok;
    ok = expect(!parse(R"([[[)").has_value(), "an unterminated array is refused") && ok;
    ok = expect(parse(R"("a\u00e9b")").has_value(), "a \\u escape parses") && ok;
    ok = expect(!parse(R"("a\qb")").has_value(), "an unknown escape is refused") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = test_committed_manifest_parses();
    ok = test_environment_lowering() && ok;
    ok = test_behavior_lowering() && ok;
    ok = test_malformed_json_is_refused() && ok;
    ok = test_schema_version_is_checked() && ok;
    ok = test_hook_names_are_checked() && ok;
    ok = test_unknown_lowering_is_refused() && ok;
    ok = test_duplicates_are_refused() && ok;
    ok = test_core_hook_names() && ok;
    ok = test_json_reader() && ok;
    if (!ok) {
        std::cerr << "hook_manifest: FAILED\n";
        return 1;
    }
    std::cout << "hook_manifest: ok\n";
    return 0;
}
