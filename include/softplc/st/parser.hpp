#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include "softplc/st/ast.hpp"
#include "softplc/st/token.hpp"

namespace softplc::st {

class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& message, int line, int column)
        : std::runtime_error(message + " (line " + std::to_string(line) + ", col " +
                              std::to_string(column) + ")") {}
};

// Recursive-descent parser for the Phase 1 Structured Text subset:
//   PROGRAM <name>
//   [VAR | VAR_INPUT | VAR_OUTPUT ... END_VAR]*
//   <statement>*
//   END_PROGRAM
// Statements: assignment, IF/ELSIF/ELSE/END_IF, WHILE/DO/END_WHILE.
// Expressions: arithmetic, comparison, boolean (AND/OR/XOR/NOT), parentheses, unary minus.
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    StProgramAst parseProgram();

private:
    const Token& peek(int offset = 0) const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const std::string& what);
    [[noreturn]] void error(const std::string& message) const;

    void parseVarBlock(std::vector<VarDecl>& decls);
    tags::TypeId parseTypeName();

    StmtList parseStatementList(std::initializer_list<TokenType> terminators);
    StmtPtr parseStatement();
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseAssignStatement();

    tags::Value parseLiteralValue(tags::TypeId declaredType);

    // Precedence, lowest to highest binding: OR, XOR, AND, equality (= <>),
    // relational (< > <= >=), additive (+ -), multiplicative (* /),
    // unary (NOT, unary -), primary.
    ExprPtr parseExpression();
    ExprPtr parseOr();
    ExprPtr parseXor();
    ExprPtr parseAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePrimary();

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
};

}  // namespace softplc::st
