// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/ui/style/token.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::css_module::detail {

inline constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

inline constexpr std::string_view kGlobalBlockCode = "TESSERA_CSS_MODULE_GLOBAL_BLOCK";
inline constexpr std::string_view kMalformedGlobalCode = "TESSERA_CSS_MODULE_MALFORMED_GLOBAL";
inline constexpr std::string_view kMalformedCode = "TESSERA_CSS_MODULE_MALFORMED";

// One byte-range replacement in the source text. An empty `replacement` is a deletion, which is how
// the `:global(` wrapper disappears without the rest of the file being reformatted.
struct Edit {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::string replacement;
};

struct RewriteResult {
    std::vector<ModuleClass> classes;
    std::vector<ModuleKeyframes> keyframes;
    std::string rewritten;
};

[[nodiscard]] std::string to_lower(std::string_view text);
[[nodiscard]] bool equals_ignore_case(std::string_view text, std::string_view expected);

// The token cursor over one stylesheet. It is a class rather than a set of free functions because
// every step shares the same cursor, the same edit list and the same keyframes table.
class Scanner {
public:
    Scanner(std::string_view text, std::string_view hash, std::vector<ui::style::Token> tokens);

    [[nodiscard]] std::expected<RewriteResult, ModuleScanError> run();

private:
    [[nodiscard]] const ui::style::Token& token_at(std::size_t index) const;
    [[nodiscard]] std::size_t skip_trivia(std::size_t index) const;
    // Index of the first `;`, `{` or `}` at paren/bracket depth zero, or of the Eof token.
    [[nodiscard]] std::size_t find_block_end(std::size_t index, ui::style::TokenType& stop) const;
    [[nodiscard]] std::expected<std::size_t, ModuleScanError>
    find_matching_paren(std::size_t open_index, std::size_t end) const;

    [[nodiscard]] std::expected<void, ModuleScanError> scan_rules(bool nested);
    [[nodiscard]] std::expected<void, ModuleScanError> scan_qualified_rule();
    [[nodiscard]] std::expected<void, ModuleScanError> scan_declarations();
    [[nodiscard]] std::expected<void, ModuleScanError> scan_keyframes_body();
    [[nodiscard]] std::expected<void, ModuleScanError> scan_at_rule();
    [[nodiscard]] std::expected<void, ModuleScanError> skip_block();

    [[nodiscard]] std::expected<void, ModuleScanError> rewrite_selector(std::size_t begin,
                                                                        std::size_t end);
    void rewrite_declaration(std::size_t begin, std::size_t end);
    void rewrite_animation_value(std::size_t begin, std::size_t end);

    void rename_class(const ui::style::Token& ident);
    void rename_keyframes(const ui::style::Token& ident);

    // Keyframes names are collected before the walk so a reference may precede its definition.
    [[nodiscard]] std::expected<void, ModuleScanError> pre_scan();
    [[nodiscard]] bool is_keyframe_name(std::string_view local) const;
    [[nodiscard]] std::string build_text() const;

    [[nodiscard]] ModuleScanError malformed(std::string message, std::size_t offset) const;

    std::string_view text_;
    std::string_view hash_;
    std::vector<ui::style::Token> tokens_;
    std::size_t pos_ = 0;
    std::vector<Edit> edits_;
    std::vector<ModuleClass> classes_;
    std::vector<ModuleKeyframes> keyframes_;
    std::vector<std::string> keyframe_names_;
};

} // namespace tessera::compiler::css_module::detail
