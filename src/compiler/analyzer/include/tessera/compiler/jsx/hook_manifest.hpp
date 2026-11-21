// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The built-in hook manifest (docs/specs/jsx-subset-v1.md section 7.1).
//
// The manifest is the single place that says what a library hook accepts and returns; the analyzer,
// lowering, code generator, preview evaluator and documentation all read it. A project cannot add
// entries, so the set is closed and the reader can reject anything it does not recognize rather
// than carrying it forward.
//
// This reader is the prerequisite both `environment` and `behavior` hooks need: neither existed in
// the front end before it, because nothing produced them from TSX.
namespace tessera::compiler::jsx {

// How a hook call lowers to IR.
enum class HookLoweringKind : std::uint8_t {
    // A read-only (or writable) engine-owned value: `environment[]`.
    Environment,
    // An engine behavior attached to one or more target refs: `behaviors[]` (ADR-064 item 7).
    Behavior,
};

// One parameter of a hook call.
struct HookParam {
    std::string name;
    // The TS type the analyzer checks against, and the IR type the value lowers to. Kept as text:
    // the manifest is the authority, and the analyzer compares rather than re-derives.
    std::string ts;
    std::string ir;
    // A `ref` parameter names a target the behavior attaches to; an `options` parameter is the one
    // object literal of value options. Empty for a plain value parameter.
    std::string kind;
    // The default the manifest declares, as the text the IR would carry. Empty when the field has
    // no default.
    std::string default_text;
    // An `options` parameter's fields, in manifest order. Empty for every other kind. The analyzer
    // checks a written key against this list and requires every field without a default, so a typo
    // is a diagnostic rather than a silently ignored option.
    std::vector<HookParam> fields;
};

// One output a hook returns, when it returns a record.
struct HookOutput {
    std::string name;
    std::string ts;
    std::string ir;
    std::string default_text;
};

struct HookEntry {
    std::string name;
    std::vector<HookParam> params;
    // Empty when the hook returns nothing.
    std::vector<HookOutput> outputs;
    // The `returns.ts`/`returns.ir` of a hook that returns one value rather than a record.
    std::string returns_ts;
    std::string returns_ir;
    HookLoweringKind lowering = HookLoweringKind::Environment;
    // `environment`: the engine provider name. `behavior`: the behavior registry name.
    std::string provider;
    bool writable = false;
    // `behavior` only: the behavior is exclusive, so two instances on one element never coexist.
    bool exclusive = false;
};

// Why a manifest was rejected. The message names the field, so a build error points at the file the
// author has to edit rather than at the reader.
struct ManifestError {
    enum class Code : std::uint8_t {
        MalformedJson,
        MissingSchemaVersion,
        UnsupportedSchemaVersion,
        MissingHooks,
        HookNotAnObject,
        MissingName,
        InvalidName,
        DuplicateName,
        CoreHookName,
        MissingLowering,
        UnknownLoweringKind,
        MissingProvider,
        MissingParamName,
        DuplicateParamName,
        MissingReturnType,
    };

    Code code = Code::MalformedJson;
    // The hook the error is about, when it is about one.
    std::string hook;
    // A human-readable detail, already formatted for a diagnostic.
    std::string detail;
};

// The schema version this reader understands. Any other value is refused rather than guessed at.
inline constexpr std::int64_t kManifestSchemaVersion = 1;

// The parsed manifest: the hooks in declaration order.
struct HookManifest {
    std::vector<HookEntry> hooks;

    // The entry named `name`, or nullptr.
    [[nodiscard]] const HookEntry* find(std::string_view name) const noexcept;
};

// Parses a manifest document. Returns the error rather than a partial manifest, so a caller never
// validates against half a file.
[[nodiscard]] std::optional<HookManifest> parse_hook_manifest(std::string_view text,
                                                              ManifestError* error = nullptr);

// True when `name` is one of the four core hooks, which the manifest may not redeclare.
[[nodiscard]] bool is_core_hook_name(std::string_view name) noexcept;

} // namespace tessera::compiler::jsx
