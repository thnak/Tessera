// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::ui::style {

class Tokenizer {
public:
    // Tokens hold views into source; the source must outlive the tokenizer and tokens.
    explicit Tokenizer(std::string_view source);

    [[nodiscard]] Token next();
    [[nodiscard]] std::vector<Token> tokenize();

    [[nodiscard]] std::size_t position() const {
        return position_;
    }

private:
    [[nodiscard]] bool at_end() const;
    [[nodiscard]] bool starts_ident(std::size_t position) const;
    [[nodiscard]] bool starts_name(std::size_t position) const;
    [[nodiscard]] bool starts_number(std::size_t position) const;
    [[nodiscard]] bool starts_unicode_range(std::size_t position) const;
    [[nodiscard]] bool matches(std::size_t position, std::string_view text) const;

    void consume_name();
    void consume_escape();
    void consume_number();
    void consume_unicode_range();

    std::string_view source_;
    std::size_t position_ = 0;
};

} // namespace tessera::ui::style
