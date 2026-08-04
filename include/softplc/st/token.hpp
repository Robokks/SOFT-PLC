#pragma once

#include <cstdint>
#include <string>

#include "softplc/tags/tag.hpp"
#include "softplc/tags/value.hpp"

namespace softplc::st {

enum class TokenType {
    // Literals & identifiers
    IntLiteral,
    RealLiteral,
    BoolLiteral,
    TimeLiteral,
    StringLiteral,
    AddressLiteral,
    Identifier,

    // Keywords
    KwProgram,
    KwEndProgram,
    KwVar,
    KwVarInput,
    KwVarOutput,
    KwEndVar,
    KwAt,
    KwIf,
    KwThen,
    KwElsif,
    KwElse,
    KwEndIf,
    KwWhile,
    KwDo,
    KwEndWhile,
    KwAnd,
    KwOr,
    KwXor,
    KwNot,
    KwBool,
    KwByte,
    KwInt,
    KwDint,
    KwReal,
    KwLreal,
    KwTime,
    KwString,
    KwFunctionBlock,
    KwEndFunctionBlock,
    KwFunction,
    KwEndFunction,
    KwVarTemp,
    KwDataBlock,
    KwEndDataBlock,
    KwRung,
    KwSet,
    KwReset,

    // Punctuation & operators
    Colon,
    Semicolon,
    Assign,   // :=
    LParen,
    RParen,
    Plus,
    Minus,
    Star,
    Slash,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Dot,      // .
    Comma,    // ,
    RArrow,   // =>

    EndOfFile,
};

struct Token {
    TokenType type = TokenType::EndOfFile;
    std::string text;
    int line = 1;
    int column = 1;

    std::int64_t intValue = 0;
    double realValue = 0.0;
    bool boolValue = false;
    std::string stringValue;
    tags::TimeValue timeValue{0};
    tags::Address addressValue{};
};

}  // namespace softplc::st
