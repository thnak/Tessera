// SPDX-License-Identifier: MIT

#include "tessera/compiler/jsx/hook_manifest.hpp"
#include "tessera/compiler/jsx/json.hpp"

#include <array>
#include <string>

namespace tessera::compiler::jsx {
namespace {

using json::Value;

std::nullopt_t fail(ManifestError* error, ManifestError::Code code, std::string hook,
                    std::string detail) {
    if (error != nullptr) {
        error->code = code;
        error->hook = std::move(hook);
        error->detail = std::move(detail);
    }
    return std::nullopt;
}

// A hook name is `use` followed by an uppercase letter, then any identifier characters. The rule is
// the manifest's, not the analyzer's: a name that fails it is an authoring error in the file.
bool valid_hook_name(std::string_view name) {
    if (name.size() < 4 || name.substr(0, 3) != "use") {
        return false;
    }
    const char first = name[3];
    if (first < 'A' || first > 'Z') {
        return false;
    }
    for (const char c : name) {
        const bool ok =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string string_or_empty(const Value* value) {
    if (value == nullptr) {
        return {};
    }
    const std::optional<std::string_view> text = value->as_string();
    return text ? std::string(*text) : std::string();
}

// A `default` may be a literal of any kind; the IR text is what the manifest wrote, so a boolean
// stays `true` and a string keeps its quotes.
std::string default_text(const Value* value) {
    if (value == nullptr || value->is_null()) {
        return {};
    }
    if (const std::optional<bool> boolean = value->as_bool()) {
        return *boolean ? "true" : "false";
    }
    if (const std::optional<double> number = value->as_number()) {
        return std::to_string(*number);
    }
    if (const std::optional<std::string_view> text = value->as_string()) {
        return std::string(*text);
    }
    return {};
}

std::optional<HookParam> parse_param(const Value& value, std::string_view hook,
                                     ManifestError* error) {
    const Value::Object* members = value.as_object();
    if (members == nullptr) {
        fail(error, ManifestError::Code::MissingParamName, std::string(hook),
             "a `params` entry is not an object");
        return std::nullopt;
    }
    HookParam param;
    param.name = string_or_empty(value.find("name"));
    if (param.name.empty()) {
        fail(error, ManifestError::Code::MissingParamName, std::string(hook),
             "a `params` entry has no `name`");
        return std::nullopt;
    }
    param.ts = string_or_empty(value.find("ts"));
    param.ir = string_or_empty(value.find("ir"));
    param.kind = string_or_empty(value.find("kind"));
    param.default_text = default_text(value.find("default"));
    return param;
}

std::optional<HookOutput> parse_output(const Value& value, std::string_view hook,
                                       ManifestError* error) {
    if (value.as_object() == nullptr) {
        fail(error, ManifestError::Code::MissingReturnType, std::string(hook),
             "a `returns.fields` entry is not an object");
        return std::nullopt;
    }
    HookOutput output;
    output.name = string_or_empty(value.find("name"));
    if (output.name.empty()) {
        fail(error, ManifestError::Code::MissingReturnType, std::string(hook),
             "a `returns.fields` entry has no `name`");
        return std::nullopt;
    }
    output.ts = string_or_empty(value.find("ts"));
    output.ir = string_or_empty(value.find("ir"));
    output.default_text = default_text(value.find("default"));
    return output;
}

std::optional<HookEntry> parse_hook(const Value& value, ManifestError* error) {
    if (value.as_object() == nullptr) {
        fail(error, ManifestError::Code::HookNotAnObject, {}, "a `hooks` entry is not an object");
        return std::nullopt;
    }
    HookEntry hook;
    hook.name = string_or_empty(value.find("name"));
    if (hook.name.empty()) {
        fail(error, ManifestError::Code::MissingName, {}, "a hook has no `name`");
        return std::nullopt;
    }
    if (!valid_hook_name(hook.name)) {
        fail(error, ManifestError::Code::InvalidName, hook.name,
             "`" + hook.name + "` is not `use` followed by an uppercase letter");
        return std::nullopt;
    }
    if (is_core_hook_name(hook.name)) {
        fail(error, ManifestError::Code::CoreHookName, hook.name,
             "`" + hook.name + "` is a core hook and may not be redeclared");
        return std::nullopt;
    }

    if (const Value* params = value.find("params"); params != nullptr) {
        const Value::Array* entries = params->as_array();
        if (entries == nullptr) {
            fail(error, ManifestError::Code::MissingParamName, hook.name,
                 "`params` is not an array");
            return std::nullopt;
        }
        for (const Value& entry : *entries) {
            std::optional<HookParam> param = parse_param(entry, hook.name, error);
            if (!param) {
                return std::nullopt;
            }
            for (const HookParam& existing : hook.params) {
                if (existing.name == param->name) {
                    fail(error, ManifestError::Code::DuplicateParamName, hook.name,
                         "`" + param->name + "` is declared twice");
                    return std::nullopt;
                }
            }
            hook.params.push_back(std::move(*param));
        }
    }

    if (const Value* returns = value.find("returns"); returns != nullptr) {
        hook.returns_ts = string_or_empty(returns->find("ts"));
        hook.returns_ir = string_or_empty(returns->find("ir"));
        if (const Value* fields = returns->find("fields"); fields != nullptr) {
            const Value::Array* entries = fields->as_array();
            if (entries == nullptr) {
                fail(error, ManifestError::Code::MissingReturnType, hook.name,
                     "`returns.fields` is not an array");
                return std::nullopt;
            }
            for (const Value& entry : *entries) {
                std::optional<HookOutput> output = parse_output(entry, hook.name, error);
                if (!output) {
                    return std::nullopt;
                }
                hook.outputs.push_back(std::move(*output));
            }
        }
    }

    const Value* lowering = value.find("lowering");
    if (lowering == nullptr || lowering->as_object() == nullptr) {
        fail(error, ManifestError::Code::MissingLowering, hook.name, "the hook has no `lowering`");
        return std::nullopt;
    }
    const std::string kind = string_or_empty(lowering->find("kind"));
    if (kind == "environment") {
        hook.lowering = HookLoweringKind::Environment;
        hook.provider = string_or_empty(lowering->find("provider"));
        if (const std::optional<bool> writable = lowering->find("writable") != nullptr
                                                     ? lowering->find("writable")->as_bool()
                                                     : std::nullopt) {
            hook.writable = *writable;
        }
    } else if (kind == "behavior") {
        hook.lowering = HookLoweringKind::Behavior;
        hook.provider = string_or_empty(lowering->find("behavior"));
        if (const Value* exclusive = lowering->find("exclusive"); exclusive != nullptr) {
            hook.exclusive = exclusive->as_bool().value_or(false);
        }
    } else {
        fail(error, ManifestError::Code::UnknownLoweringKind, hook.name,
             "`" + kind + "` is not a lowering kind this front end knows");
        return std::nullopt;
    }
    if (hook.provider.empty()) {
        fail(error, ManifestError::Code::MissingProvider, hook.name,
             "the `" + kind + "` lowering names no provider");
        return std::nullopt;
    }
    return hook;
}

} // namespace

const HookEntry* HookManifest::find(std::string_view name) const noexcept {
    for (const HookEntry& hook : hooks) {
        if (hook.name == name) {
            return &hook;
        }
    }
    return nullptr;
}

bool is_core_hook_name(std::string_view name) noexcept {
    constexpr std::array<std::string_view, 4> kCore{"useState", "useRef", "useEffect",
                                                    "useAnimatedValue"};
    for (const std::string_view core : kCore) {
        if (core == name) {
            return true;
        }
    }
    return false;
}

std::optional<HookManifest> parse_hook_manifest(std::string_view text, ManifestError* error) {
    json::Error json_error;
    const std::optional<Value> document = json::parse(text, &json_error);
    if (!document) {
        fail(error, ManifestError::Code::MalformedJson, {},
             "the manifest is not valid JSON at byte " + std::to_string(json_error.offset));
        return std::nullopt;
    }
    if (document->as_object() == nullptr) {
        fail(error, ManifestError::Code::MalformedJson, {}, "the manifest is not a JSON object");
        return std::nullopt;
    }

    const Value* version = document->find("schemaVersion");
    if (version == nullptr) {
        fail(error, ManifestError::Code::MissingSchemaVersion, {},
             "the manifest has no `schemaVersion`");
        return std::nullopt;
    }
    const std::optional<double> number = version->as_number();
    if (!number || static_cast<std::int64_t>(*number) != kManifestSchemaVersion) {
        fail(error, ManifestError::Code::UnsupportedSchemaVersion, {},
             "this front end reads `schemaVersion` " + std::to_string(kManifestSchemaVersion));
        return std::nullopt;
    }

    const Value* hooks = document->find("hooks");
    if (hooks == nullptr || hooks->as_array() == nullptr) {
        fail(error, ManifestError::Code::MissingHooks, {}, "the manifest has no `hooks` array");
        return std::nullopt;
    }

    HookManifest manifest;
    for (const Value& entry : *hooks->as_array()) {
        std::optional<HookEntry> hook = parse_hook(entry, error);
        if (!hook) {
            return std::nullopt;
        }
        if (manifest.find(hook->name) != nullptr) {
            fail(error, ManifestError::Code::DuplicateName, hook->name,
                 "`" + hook->name + "` is declared twice");
            return std::nullopt;
        }
        manifest.hooks.push_back(std::move(*hook));
    }
    return manifest;
}

} // namespace tessera::compiler::jsx
