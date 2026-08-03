#include "softplc/st/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace softplc::st {

namespace {

std::string toUpper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

const std::unordered_map<std::string, TokenType>& keywordTable() {
    static const std::unordered_map<std::string, TokenType> table = {
        {"PROGRAM", TokenType::KwProgram},   {"END_PROGRAM", TokenType::KwEndProgram},
        {"VAR", TokenType::KwVar},           {"VAR_INPUT", TokenType::KwVarInput},
        {"VAR_OUTPUT", TokenType::KwVarOutput}, {"END_VAR", TokenType::KwEndVar},
        {"AT", TokenType::KwAt},             {"IF", TokenType::KwIf},
        {"THEN", TokenType::KwThen},         {"ELSIF", TokenType::KwElsif},
        {"ELSE", TokenType::KwElse},         {"END_IF", TokenType::KwEndIf},
        {"WHILE", TokenType::KwWhile},       {"DO", TokenType::KwDo},
        {"END_WHILE", TokenType::KwEndWhile}, {"AND", TokenType::KwAnd},
        {"OR", TokenType::KwOr},             {"XOR", TokenType::KwXor},
        {"NOT", TokenType::KwNot},           {"BOOL", TokenType::KwBool},
        {"BYTE", TokenType::KwByte},         {"INT", TokenType::KwInt},
        {"DINT", TokenType::KwDint},         {"REAL", TokenType::KwReal},
        {"LREAL", TokenType::KwLreal},       {"TIME", TokenType::KwTime},
        {"STRING", TokenType::KwString},
    };
    return table;
}

std::int64_t timeUnitToMillis(const std::string& unit) {
    if (unit == "ms") return 1;
    if (unit == "s") return 1000;
    if (unit == "m") return 60'000;
    if (unit == "h") return 3'600'000;
    if (unit == "d") return 86'400'000;
    return -1;
}

}  // namespace

Lexer::Lexer(std::string_view source) : source_(source) {}

char Lexer::peek(int offset) const {
    const std::size_t idx = pos_ + static_cast<std::size_t>(offset);
    if (idx >= source_.size()) return '\0';
    return source_[idx];
}

char Lexer::advance() {
    const char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (peek() != expected) return false;
    advance();
    return true;
}

void Lexer::error(const std::string& message) const { throw LexError(message, line_, column_); }

Token Lexer::makeToken(TokenType type, std::string text) const {
    Token tok;
    tok.type = type;
    tok.text = std::move(text);
    tok.line = line_;
    tok.column = column_;
    return tok;
}

void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (pos_ < source_.size() && peek() != '\n') advance();
        } else if (c == '(' && peek(1) == '*') {
            advance();
            advance();
            while (pos_ < source_.size() && !(peek() == '*' && peek(1) == ')')) advance();
            if (pos_ >= source_.size()) error("unterminated comment");
            advance();
            advance();
        } else {
            break;
        }
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token tok = nextToken();
        const bool isEof = tok.type == TokenType::EndOfFile;
        tokens.push_back(std::move(tok));
        if (isEof) break;
    }
    return tokens;
}

Token Lexer::nextToken() {
    skipWhitespaceAndComments();
    if (pos_ >= source_.size()) {
        return makeToken(TokenType::EndOfFile, "");
    }

    const char c = peek();
    const int startLine = line_;
    const int startColumn = column_;

    if (std::isdigit(static_cast<unsigned char>(c))) {
        return lexNumber();
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        const std::size_t save = pos_;
        const int saveLine = line_;
        const int saveColumn = column_;

        std::string word;
        while (pos_ < source_.size() &&
               (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
            word.push_back(advance());
        }
        const std::string upper = toUpper(word);
        if ((upper == "T" || upper == "TIME") && peek() == '#') {
            advance();  // consume '#'
            return lexTimeLiteral(startLine, startColumn);
        }
        pos_ = save;
        line_ = saveLine;
        column_ = saveColumn;
        return lexIdentifierOrKeyword();
    }
    if (c == '\'') {
        return lexString();
    }
    if (c == '%') {
        return lexAddress();
    }

    switch (c) {
        case ':':
            advance();
            if (match('=')) return makeToken(TokenType::Assign, ":=");
            return makeToken(TokenType::Colon, ":");
        case ';':
            advance();
            return makeToken(TokenType::Semicolon, ";");
        case '(':
            advance();
            return makeToken(TokenType::LParen, "(");
        case ')':
            advance();
            return makeToken(TokenType::RParen, ")");
        case '+':
            advance();
            return makeToken(TokenType::Plus, "+");
        case '-':
            advance();
            return makeToken(TokenType::Minus, "-");
        case '*':
            advance();
            return makeToken(TokenType::Star, "*");
        case '/':
            advance();
            return makeToken(TokenType::Slash, "/");
        case '=':
            advance();
            return makeToken(TokenType::Eq, "=");
        case '<':
            advance();
            if (match('>')) return makeToken(TokenType::Ne, "<>");
            if (match('=')) return makeToken(TokenType::Le, "<=");
            return makeToken(TokenType::Lt, "<");
        case '>':
            advance();
            if (match('=')) return makeToken(TokenType::Ge, ">=");
            return makeToken(TokenType::Gt, ">");
        default:
            error(std::string("unexpected character '") + c + "'");
    }
}

Token Lexer::lexNumber() {
    const int startLine = line_;
    const int startColumn = column_;
    std::string text;
    bool isReal = false;

    while (std::isdigit(static_cast<unsigned char>(peek()))) text.push_back(advance());
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isReal = true;
        text.push_back(advance());  // '.'
        while (std::isdigit(static_cast<unsigned char>(peek()))) text.push_back(advance());
    }

    Token tok = makeToken(isReal ? TokenType::RealLiteral : TokenType::IntLiteral, text);
    tok.line = startLine;
    tok.column = startColumn;
    if (isReal) {
        tok.realValue = std::stod(text);
    } else {
        tok.intValue = std::stoll(text);
    }
    return tok;
}

Token Lexer::lexTimeLiteral(int startLine, int startColumn) {
    std::int64_t totalMillis = 0;
    bool any = false;

    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        std::string digits;
        while (std::isdigit(static_cast<unsigned char>(peek()))) digits.push_back(advance());

        std::string unit;
        while (std::isalpha(static_cast<unsigned char>(peek()))) unit.push_back(advance());

        const std::int64_t perUnit = timeUnitToMillis(unit);
        if (perUnit < 0) error("invalid TIME unit '" + unit + "'");
        totalMillis += std::stoll(digits) * perUnit;
        any = true;

        if (peek() == '_') advance();  // allow T#1h_30m style separators
    }

    if (!any) error("invalid TIME literal");

    Token tok = makeToken(TokenType::TimeLiteral, "");
    tok.line = startLine;
    tok.column = startColumn;
    tok.timeValue = tags::TimeValue(totalMillis);
    return tok;
}

Token Lexer::lexIdentifierOrKeyword() {
    const int startLine = line_;
    const int startColumn = column_;
    std::string word;
    while (pos_ < source_.size() &&
           (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        word.push_back(advance());
    }

    const std::string upper = toUpper(word);
    if (upper == "TRUE" || upper == "FALSE") {
        Token tok = makeToken(TokenType::BoolLiteral, word);
        tok.line = startLine;
        tok.column = startColumn;
        tok.boolValue = (upper == "TRUE");
        return tok;
    }

    const auto& keywords = keywordTable();
    auto it = keywords.find(upper);
    if (it != keywords.end()) {
        Token tok = makeToken(it->second, word);
        tok.line = startLine;
        tok.column = startColumn;
        return tok;
    }

    Token tok = makeToken(TokenType::Identifier, word);
    tok.line = startLine;
    tok.column = startColumn;
    return tok;
}

Token Lexer::lexString() {
    const int startLine = line_;
    const int startColumn = column_;
    advance();  // opening quote
    std::string value;
    while (pos_ < source_.size() && peek() != '\'') {
        value.push_back(advance());
    }
    if (pos_ >= source_.size()) error("unterminated string literal");
    advance();  // closing quote

    Token tok = makeToken(TokenType::StringLiteral, value);
    tok.line = startLine;
    tok.column = startColumn;
    tok.stringValue = value;
    return tok;
}

Token Lexer::lexAddress() {
    const int startLine = line_;
    const int startColumn = column_;
    advance();  // '%'

    tags::MemoryArea area = tags::MemoryArea::None;
    switch (std::toupper(static_cast<unsigned char>(peek()))) {
        case 'I':
            area = tags::MemoryArea::Input;
            break;
        case 'Q':
            area = tags::MemoryArea::Output;
            break;
        case 'M':
            area = tags::MemoryArea::Memory;
            break;
        default:
            error("invalid direct address: expected I, Q or M after '%'");
    }
    advance();  // area letter

    // Optional size designator (X = bit, B = byte, W = word, D = dword, L = lword);
    // Phase 1 only models byte+bit granularity, so the designator is accepted and
    // otherwise ignored.
    const char sizeDesignator =
        static_cast<char>(std::toupper(static_cast<unsigned char>(peek())));
    if (sizeDesignator == 'X' || sizeDesignator == 'B' || sizeDesignator == 'W' ||
        sizeDesignator == 'D' || sizeDesignator == 'L') {
        advance();
    }

    if (!std::isdigit(static_cast<unsigned char>(peek()))) {
        error("invalid direct address: expected byte offset digits");
    }
    std::string byteDigits;
    while (std::isdigit(static_cast<unsigned char>(peek()))) byteDigits.push_back(advance());

    std::uint8_t bitOffset = tags::Address::kNoBit;
    if (peek() == '.') {
        advance();
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            error("invalid direct address: expected bit offset digits after '.'");
        }
        std::string bitDigits;
        while (std::isdigit(static_cast<unsigned char>(peek()))) bitDigits.push_back(advance());
        bitOffset = static_cast<std::uint8_t>(std::stoi(bitDigits));
    }

    Token tok = makeToken(TokenType::AddressLiteral, "");
    tok.line = startLine;
    tok.column = startColumn;
    tok.addressValue =
        tags::Address{.area = area, .byteOffset = static_cast<std::uint32_t>(std::stoul(byteDigits)),
                      .bitOffset = bitOffset};
    return tok;
}

}  // namespace softplc::st
