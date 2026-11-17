// SPDX-License-Identifier: MIT

#include "module_rewrite.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/ui/style/tokenizer.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::css_module {

std::expected<ModuleScan, ModuleScanError> scan_module_css(std::string_view project_relative_path,
                                                           std::string_view text) {
    const std::string hash = module_hash(project_relative_path);

    // The tokenizer is the stylesheet engine's own, so the compiler and the runtime agree on what a
    // token is; only the scene-free half of `tessera_style` is linked in for it.
    ui::style::Tokenizer tokenizer(text);
    std::vector<ui::style::Token> tokens = tokenizer.tokenize();

    detail::Scanner scanner(text, hash, std::move(tokens));
    auto result = scanner.run();
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return ModuleScan{hash, std::move(result->classes), std::move(result->keyframes),
                      std::move(result->rewritten)};
}

} // namespace tessera::compiler::css_module
