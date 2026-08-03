#include "softplc/st/lexer.hpp"

#include <gtest/gtest.h>

using namespace softplc::st;

TEST(LexerTest, TokenizesKeywordsAndPunctuation) {
    Lexer lexer("PROGRAM Foo VAR END_VAR END_PROGRAM");
    auto tokens = lexer.tokenize();

    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::KwProgram);
    EXPECT_EQ(tokens[1].type, TokenType::Identifier);
    EXPECT_EQ(tokens[1].text, "Foo");
    EXPECT_EQ(tokens[2].type, TokenType::KwVar);
    EXPECT_EQ(tokens[3].type, TokenType::KwEndVar);
    EXPECT_EQ(tokens[4].type, TokenType::KwEndProgram);
    EXPECT_EQ(tokens.back().type, TokenType::EndOfFile);
}

TEST(LexerTest, TokenizesIntAndRealLiterals) {
    Lexer lexer("42 3.14");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::IntLiteral);
    EXPECT_EQ(tokens[0].intValue, 42);
    EXPECT_EQ(tokens[1].type, TokenType::RealLiteral);
    EXPECT_DOUBLE_EQ(tokens[1].realValue, 3.14);
}

TEST(LexerTest, TokenizesBoolLiterals) {
    Lexer lexer("TRUE FALSE");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::BoolLiteral);
    EXPECT_TRUE(tokens[0].boolValue);
    EXPECT_EQ(tokens[1].type, TokenType::BoolLiteral);
    EXPECT_FALSE(tokens[1].boolValue);
}

TEST(LexerTest, TokenizesTimeLiteral) {
    Lexer lexer("T#1h30m");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::TimeLiteral);
    EXPECT_EQ(tokens[0].timeValue.count(), 90 * 60 * 1000);
}

TEST(LexerTest, TokenizesStringLiteral) {
    Lexer lexer("'hello world'");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::StringLiteral);
    EXPECT_EQ(tokens[0].stringValue, "hello world");
}

TEST(LexerTest, TokenizesDirectAddress) {
    Lexer lexer("%Q0.0 %IW4 %M10");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::AddressLiteral);
    EXPECT_EQ(tokens[0].addressValue.area, softplc::tags::MemoryArea::Output);
    EXPECT_EQ(tokens[0].addressValue.byteOffset, 0u);
    EXPECT_EQ(tokens[0].addressValue.bitOffset, 0u);

    EXPECT_EQ(tokens[1].addressValue.area, softplc::tags::MemoryArea::Input);
    EXPECT_EQ(tokens[1].addressValue.byteOffset, 4u);
    EXPECT_EQ(tokens[1].addressValue.bitOffset, softplc::tags::Address::kNoBit);

    EXPECT_EQ(tokens[2].addressValue.area, softplc::tags::MemoryArea::Memory);
    EXPECT_EQ(tokens[2].addressValue.byteOffset, 10u);
}

TEST(LexerTest, SkipsCommentsAndWhitespace) {
    Lexer lexer("(* comment *) // line comment\nIF");
    auto tokens = lexer.tokenize();
    EXPECT_EQ(tokens[0].type, TokenType::KwIf);
}

TEST(LexerTest, ThrowsOnUnterminatedString) {
    Lexer lexer("'unterminated");
    EXPECT_THROW(lexer.tokenize(), LexError);
}

TEST(LexerTest, ThrowsOnUnexpectedCharacter) {
    Lexer lexer("@");
    EXPECT_THROW(lexer.tokenize(), LexError);
}
