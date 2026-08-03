#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "softplc/st/token.hpp"

namespace softplc::st {

class LexError : public std::runtime_error {
public:
    LexError(const std::string& message, int line, int column)
        : std::runtime_error(message + " (line " + std::to_string(line) + ", col " +
                              std::to_string(column) + ")") {}
};

// Hand-written single-pass lexer for the Phase 1 Structured Text subset.
class Lexer {
public:
    explicit Lexer(std::string_view source);

    // Tokenizes the whole source and returns the token stream, terminated by a
    // single EndOfFile token.
    std::vector<Token> tokenize();

private:
    char peek(int offset = 0) const;
    char advance();
    bool match(char expected);
    [[noreturn]] void error(const std::string& message) const;

    void skipWhitespaceAndComments();
    Token nextToken();
    Token makeToken(TokenType type, std::string text) const;

    Token lexNumber();
    Token lexTimeLiteral(int startLine, int startColumn);
    Token lexIdentifierOrKeyword();
    Token lexString();
    Token lexAddress();

    std::string source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
};

}  // namespace softplc::st
