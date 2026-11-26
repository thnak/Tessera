// SPDX-License-Identifier: MIT

// ADR-058 typing rule 1 (#547): `Partial`, `Required`, `Pick`, `Omit` and the intersection of
// records are absorbed at compile time. Each becomes the one object type of the resulting members,
// so every later pass (rules, lowering, codegen) sees only the `type X = { ... }` it already knows
// and the IR does not change. The records are resolved within the file: a name that is not a record
// declared here is reported with the fix instead of being half-expanded.

#include "parser_internal.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::compiler::jsx {
namespace {

using Report = std::function<void(std::string_view, std::string, Span, std::string)>;

TypePtr clone_type(const TypeNode& source) {
    auto copy = std::make_unique<TypeNode>();
    copy->kind = source.kind;
    copy->span = source.span;
    copy->height = source.height;
    copy->text = source.text;
    copy->param_names = source.param_names;
    for (const TypePtr& child : source.children) {
        copy->children.push_back(child ? clone_type(*child) : nullptr);
    }
    for (const TypeMember& member : source.members) {
        copy->members.push_back(TypeMember{member.name, member.optional, member.span,
                                           member.type ? clone_type(*member.type) : nullptr});
    }
    return copy;
}

bool is_utility(std::string_view name) {
    return name == "Partial" || name == "Required" || name == "Pick" || name == "Omit";
}

class Expander {
public:
    Expander(Module& module, Report report)
        : module_(module),
          report_(std::move(report)) {}

    void run() {
        for (ModuleItem& item : module_.items) {
            if (item.kind == ItemKind::TypeAlias && item.type) {
                aliases_[item.name] = &item;
            }
        }
        for (ModuleItem& item : module_.items) {
            if (item.kind == ItemKind::TypeAlias && item.type) {
                expand_alias(item);
            } else if (item.kind == ItemKind::Component && item.props_type) {
                expand(item.props_type);
            }
        }
    }

private:
    void expand_alias(ModuleItem& item) {
        if (done_.contains(&item) || !visiting_.insert(&item).second) {
            return; // done, or a cycle: the lowering reports a cycle where the developer wrote it
        }
        expand(item.type);
        visiting_.erase(&item);
        done_.insert(&item);
    }

    // The object type `type` stands for, after expansion: an inline object, or an alias that is
    // one.
    const TypeNode* object_of(const TypeNode& type) {
        if (type.kind == TypeKind::Object) {
            return &type;
        }
        if (type.kind == TypeKind::Name && type.children.empty()) {
            const auto found = aliases_.find(type.text);
            if (found != aliases_.end()) {
                expand_alias(*found->second);
                const TypeNode* body = found->second->type.get();
                return body != nullptr && body->kind == TypeKind::Object ? body : nullptr;
            }
        }
        return nullptr;
    }

    void expand(TypePtr& type) {
        if (!type) {
            return;
        }
        for (TypePtr& child : type->children) {
            expand(child);
        }
        for (TypeMember& member : type->members) {
            expand(member.type);
        }
        if (type->kind == TypeKind::Intersection) {
            expand_intersection(type);
        } else if (type->kind == TypeKind::Name && is_utility(type->text) &&
                   !type->children.empty()) {
            expand_utility(type);
        }
    }

    void expand_intersection(TypePtr& type) {
        auto merged = std::make_unique<TypeNode>();
        merged->kind = TypeKind::Object;
        merged->span = type->span;
        merged->height = type->height;
        for (const TypePtr& child : type->children) {
            const TypeNode* body = child ? object_of(*child) : nullptr;
            if (body == nullptr) {
                report_("TESSERA_JSX_TYPE_INTERSECTION",
                        "only record types declared in this file can be intersected",
                        child ? child->span : type->span, "write one `type` with all the members");
                return;
            }
            for (const TypeMember& member : body->members) {
                const bool taken = std::ranges::any_of(
                    merged->members, [&](const TypeMember& m) { return m.name == member.name; });
                if (taken) {
                    report_("TESSERA_JSX_TYPE_INTERSECTION",
                            "member `" + member.name +
                                "` is declared by more than one intersected type",
                            member.span, "keep one declaration of `" + member.name + "`");
                    return;
                }
                merged->members.push_back(TypeMember{member.name, member.optional, member.span,
                                                     clone_type(*member.type)});
            }
        }
        type = std::move(merged);
    }

    // `"a" | "b"` or `"a"` as the key list of Pick/Omit.
    static bool collect_keys(const TypeNode& type, std::vector<std::string>& keys) {
        if (type.kind == TypeKind::StringLiteral) {
            keys.push_back(type.text);
            return true;
        }
        if (type.kind != TypeKind::Union) {
            return false;
        }
        return std::ranges::all_of(type.children, [&](const TypePtr& child) {
            return child && child->kind == TypeKind::StringLiteral &&
                   (keys.push_back(child->text), true);
        });
    }

    void expand_utility(TypePtr& type) {
        const std::string& name = type->text;
        const bool keyed = name == "Pick" || name == "Omit";
        if (type->children.size() != (keyed ? 2U : 1U)) {
            report_("TESSERA_JSX_TYPE_UTILITY",
                    "`" + name + "` takes " + (keyed ? "a record and its keys" : "one record"),
                    type->span,
                    keyed ? "write `" + name + "<Item, \"a\" | \"b\">`"
                          : "write `" + name + "<Item>`");
            return;
        }
        const TypeNode* body = type->children[0] ? object_of(*type->children[0]) : nullptr;
        if (body == nullptr) {
            report_("TESSERA_JSX_TYPE_UTILITY",
                    "`" + name + "` needs a record type declared in this file", type->span,
                    "declare `type Item = { ... }` in this file, or write the members out");
            return;
        }
        std::vector<std::string> keys;
        if (keyed) {
            if (!type->children[1] || !collect_keys(*type->children[1], keys)) {
                report_("TESSERA_JSX_TYPE_UTILITY",
                        "the keys of `" + name + "` must be string literals",
                        type->children[1] ? type->children[1]->span : type->span,
                        "write the keys as `\"a\" | \"b\"`");
                return;
            }
            for (const std::string& key : keys) {
                const bool known = std::ranges::any_of(
                    body->members, [&](const TypeMember& m) { return m.name == key; });
                if (!known) {
                    report_("TESSERA_JSX_TYPE_UTILITY",
                            "`" + key + "` is not a member of the record", type->children[1]->span,
                            "name a member of the record");
                    return;
                }
            }
        }
        auto result = std::make_unique<TypeNode>();
        result->kind = TypeKind::Object;
        result->span = type->span;
        result->height = type->height;
        for (const TypeMember& member : body->members) {
            const bool listed = std::ranges::find(keys, member.name) != keys.end();
            if ((name == "Pick" && !listed) || (name == "Omit" && listed)) {
                continue;
            }
            TypeMember copy{member.name, member.optional, member.span, clone_type(*member.type)};
            if (name == "Partial") {
                copy.optional = true;
            } else if (name == "Required") {
                copy.optional = false;
            }
            result->members.push_back(std::move(copy));
        }
        type = std::move(result);
    }

    Module& module_;
    Report report_;
    std::unordered_map<std::string, ModuleItem*> aliases_;
    std::unordered_set<const ModuleItem*> done_;
    std::unordered_set<const ModuleItem*> visiting_;
};

} // namespace

void Parser::expand_type_forms(Module& module) {
    Expander(module, [this](std::string_view code, std::string message, Span span,
                            std::string fix) {
        report(code, std::move(message), span, std::move(fix));
    }).run();
}

} // namespace tessera::compiler::jsx
