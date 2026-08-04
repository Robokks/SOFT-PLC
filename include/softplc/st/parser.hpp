#pragma once

#include <optional>
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

// Recursive-descent parser for the Structured Text subset:
//   [FUNCTION_BLOCK <name> ... END_FUNCTION_BLOCK]*
//   [FUNCTION <name> ... END_FUNCTION]*
//   [DATA_BLOCK <name> VAR ... END_VAR END_DATA_BLOCK]*
//   PROGRAM <name>
//     [VAR | VAR_INPUT | VAR_OUTPUT ... END_VAR]*
//     <statement>*
//   END_PROGRAM
// (exactly one PROGRAM per compilation unit; the POU/DB definitions above may
// appear in any order/count before or after it)
// Statements: assignment, IF/ELSIF/ELSE/END_IF, WHILE/DO/END_WHILE, calls
// (`InstanceOrFcName(Param := expr, Param => target);`), and ladder-style rungs
// (`RUNG <bool-expr> => [SET|RESET] target (, [SET|RESET] target)*;`).
// Expressions: arithmetic, comparison, boolean (AND/OR/XOR/NOT), parentheses, unary
// minus. Identifiers may be dotted (`DB1.Speed`, `Motor1.Running`).
class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // Parses a full compilation unit: any number of FUNCTION_BLOCK/FUNCTION/
    // DATA_BLOCK definitions plus exactly one PROGRAM.
    CompilationUnit parseCompilationUnit();

    // Parses a single `PROGRAM ... END_PROGRAM`, starting at the current position and
    // expecting nothing else to follow. Exposed separately (in addition to
    // parseCompilationUnit()) so existing single-PROGRAM-only tests/tools keep working.
    StProgramAst parseProgram();

    // Parses a sequence of FUNCTION_BLOCK/FUNCTION definitions only -- no DATA_BLOCK,
    // no PROGRAM required. Used to load the standard FB library (st/standard_fbs.hpp)
    // as ordinary PouAsts merged into a user CompilationUnit's pous by
    // StProgram::load(), rather than a special-cased "intrinsic" FB mechanism.
    std::vector<PouAst> parsePouLibrary();

private:
    // Which kind of VAR block is being parsed -- controls what's allowed inside it
    // (FB instance declarations, persistent VAR, VAR_TEMP).
    enum class VarBlockContext { Program, FunctionBlock, Function, DataBlock };

    const Token& peek(int offset = 0) const;
    const Token& advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    const Token& expect(TokenType type, const std::string& what);
    [[noreturn]] void error(const std::string& message) const;

    PouAst parsePouDef(bool isFunction);
    DataBlockAst parseDataBlockDef();

    void parseVarBlock(std::vector<VarDecl>& decls, VarBlockContext context);
    std::optional<tags::TypeId> tryParseElementaryType();

    // Glues Identifier (Dot Identifier)* into one dotted name, e.g. "DB1.Speed".
    std::string parseDottedIdentifier();

    StmtList parseStatementList(std::initializer_list<TokenType> terminators);
    StmtPtr parseStatement();
    StmtPtr parseIfStatement();
    StmtPtr parseWhileStatement();
    StmtPtr parseRungStatement();
    RungOutput parseRungOutput();
    // Parses a dotted identifier, then dispatches to an assignment (':=') or a call
    // ('(') based on what follows.
    StmtPtr parseAssignOrCallStatement();
    StmtPtr parseCallStatementRest(std::string calleeName);
    CallArg parseCallArg();

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
