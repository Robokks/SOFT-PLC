#include "softplc/st/parser.hpp"

#include <type_traits>
#include <utility>

namespace softplc::st {

namespace {

tags::Value negateLiteral(const tags::Value& v) {
    return std::visit(
        [](auto&& x) -> tags::Value {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
                throw std::runtime_error("cannot negate a BOOL or STRING literal");
            } else {
                // Cast back to T explicitly: integer promotion (e.g. uint8_t/int16_t -> int)
                // would otherwise silently change which variant alternative gets constructed.
                return static_cast<T>(-x);
            }
        },
        v);
}

}  // namespace

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::peek(int offset) const {
    std::size_t idx = pos_ + static_cast<std::size_t>(offset);
    if (idx >= tokens_.size()) idx = tokens_.size() - 1;  // EndOfFile sentinel
    return tokens_[idx];
}

const Token& Parser::advance() {
    const Token& tok = peek();
    if (pos_ < tokens_.size() - 1) ++pos_;
    return tok;
}

bool Parser::check(TokenType type) const { return peek().type == type; }

bool Parser::match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

const Token& Parser::expect(TokenType type, const std::string& what) {
    if (!check(type)) {
        error("expected " + what + " but found '" + peek().text + "'");
    }
    return advance();
}

void Parser::error(const std::string& message) const {
    throw ParseError(message, peek().line, peek().column);
}

StProgramAst Parser::parseProgram() {
    StProgramAst ast;

    expect(TokenType::KwProgram, "PROGRAM");
    ast.name = expect(TokenType::Identifier, "program name").text;

    while (check(TokenType::KwVar) || check(TokenType::KwVarInput) ||
           check(TokenType::KwVarOutput)) {
        parseVarBlock(ast.varDecls);
    }

    ast.body = parseStatementList({TokenType::KwEndProgram});
    expect(TokenType::KwEndProgram, "END_PROGRAM");

    return ast;
}

void Parser::parseVarBlock(std::vector<VarDecl>& decls) {
    VarKind kind;
    if (match(TokenType::KwVar)) {
        kind = VarKind::Var;
    } else if (match(TokenType::KwVarInput)) {
        kind = VarKind::VarInput;
    } else if (match(TokenType::KwVarOutput)) {
        kind = VarKind::VarOutput;
    } else {
        error("expected VAR, VAR_INPUT or VAR_OUTPUT");
    }

    while (!check(TokenType::KwEndVar)) {
        VarDecl decl;
        decl.kind = kind;
        decl.name = expect(TokenType::Identifier, "variable name").text;

        if (match(TokenType::KwAt)) {
            decl.address = expect(TokenType::AddressLiteral, "direct address").addressValue;
        }

        expect(TokenType::Colon, "':'");
        decl.type = parseTypeName();

        if (match(TokenType::Assign)) {
            decl.initialValue = parseLiteralValue(decl.type);
        }

        expect(TokenType::Semicolon, "';'");
        decls.push_back(std::move(decl));
    }
    expect(TokenType::KwEndVar, "END_VAR");
}

tags::TypeId Parser::parseTypeName() {
    const Token& tok = advance();
    switch (tok.type) {
        case TokenType::KwBool:
            return tags::TypeId::Bool;
        case TokenType::KwByte:
            return tags::TypeId::Byte;
        case TokenType::KwInt:
            return tags::TypeId::Int;
        case TokenType::KwDint:
            return tags::TypeId::DInt;
        case TokenType::KwReal:
            return tags::TypeId::Real;
        case TokenType::KwLreal:
            return tags::TypeId::LReal;
        case TokenType::KwTime:
            return tags::TypeId::Time;
        case TokenType::KwString:
            return tags::TypeId::String;
        default:
            error("expected a type name (BOOL, BYTE, INT, DINT, REAL, LREAL, TIME, STRING)");
    }
}

tags::Value Parser::parseLiteralValue(tags::TypeId declaredType) {
    const bool negative = match(TokenType::Minus);
    const Token& tok = advance();

    tags::Value value;
    switch (tok.type) {
        case TokenType::IntLiteral:
            switch (declaredType) {
                case tags::TypeId::Byte:
                    value = static_cast<std::uint8_t>(tok.intValue);
                    break;
                case tags::TypeId::Int:
                    value = static_cast<std::int16_t>(tok.intValue);
                    break;
                case tags::TypeId::DInt:
                    value = static_cast<std::int32_t>(tok.intValue);
                    break;
                case tags::TypeId::Real:
                    value = static_cast<float>(tok.intValue);
                    break;
                case tags::TypeId::LReal:
                    value = static_cast<double>(tok.intValue);
                    break;
                default:
                    error("integer literal is not valid for the declared type");
            }
            break;
        case TokenType::RealLiteral:
            if (declaredType == tags::TypeId::Real) {
                value = static_cast<float>(tok.realValue);
            } else if (declaredType == tags::TypeId::LReal) {
                value = tok.realValue;
            } else {
                error("real literal is not valid for the declared type");
            }
            break;
        case TokenType::BoolLiteral:
            if (declaredType != tags::TypeId::Bool) error("BOOL literal is not valid here");
            value = tok.boolValue;
            break;
        case TokenType::TimeLiteral:
            if (declaredType != tags::TypeId::Time) error("TIME literal is not valid here");
            value = tok.timeValue;
            break;
        case TokenType::StringLiteral:
            if (declaredType != tags::TypeId::String) error("STRING literal is not valid here");
            value = tok.stringValue;
            break;
        default:
            error("expected a literal value");
    }

    if (negative) {
        value = negateLiteral(value);
    }
    return value;
}

StmtList Parser::parseStatementList(std::initializer_list<TokenType> terminators) {
    StmtList stmts;
    while (true) {
        bool atTerminator = false;
        for (TokenType t : terminators) {
            if (check(t)) {
                atTerminator = true;
                break;
            }
        }
        if (atTerminator) break;
        stmts.push_back(parseStatement());
        // Tolerate stray/trailing semicolons after IF/WHILE blocks (common ST style,
        // e.g. "END_IF;"), even though END_IF/END_WHILE already terminate the statement.
        while (match(TokenType::Semicolon)) {
        }
    }
    return stmts;
}

StmtPtr Parser::parseStatement() {
    if (check(TokenType::KwIf)) return parseIfStatement();
    if (check(TokenType::KwWhile)) return parseWhileStatement();
    if (check(TokenType::Identifier)) return parseAssignStatement();
    error("expected a statement (assignment, IF or WHILE)");
}

StmtPtr Parser::parseIfStatement() {
    expect(TokenType::KwIf, "IF");

    auto stmt = std::make_unique<IfStmt>();

    ExprPtr cond = parseExpression();
    expect(TokenType::KwThen, "THEN");
    StmtList body =
        parseStatementList({TokenType::KwElsif, TokenType::KwElse, TokenType::KwEndIf});
    stmt->branches.push_back(IfBranch{std::move(cond), std::move(body)});

    while (match(TokenType::KwElsif)) {
        ExprPtr elsifCond = parseExpression();
        expect(TokenType::KwThen, "THEN");
        StmtList elsifBody =
            parseStatementList({TokenType::KwElsif, TokenType::KwElse, TokenType::KwEndIf});
        stmt->branches.push_back(IfBranch{std::move(elsifCond), std::move(elsifBody)});
    }

    if (match(TokenType::KwElse)) {
        stmt->elseBody = parseStatementList({TokenType::KwEndIf});
    }

    expect(TokenType::KwEndIf, "END_IF");
    return stmt;
}

StmtPtr Parser::parseWhileStatement() {
    expect(TokenType::KwWhile, "WHILE");
    ExprPtr cond = parseExpression();
    expect(TokenType::KwDo, "DO");
    StmtList body = parseStatementList({TokenType::KwEndWhile});
    expect(TokenType::KwEndWhile, "END_WHILE");
    return std::make_unique<WhileStmt>(std::move(cond), std::move(body));
}

StmtPtr Parser::parseAssignStatement() {
    const Token& nameTok = expect(TokenType::Identifier, "identifier");
    expect(TokenType::Assign, "':='");
    ExprPtr value = parseExpression();
    expect(TokenType::Semicolon, "';'");
    return std::make_unique<AssignStmt>(nameTok.text, std::move(value));
}

ExprPtr Parser::parseExpression() { return parseOr(); }

ExprPtr Parser::parseOr() {
    ExprPtr left = parseXor();
    while (match(TokenType::KwOr)) {
        ExprPtr right = parseXor();
        left = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseXor() {
    ExprPtr left = parseAnd();
    while (match(TokenType::KwXor)) {
        ExprPtr right = parseAnd();
        left = std::make_unique<BinaryExpr>(BinaryOp::Xor, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseAnd() {
    ExprPtr left = parseEquality();
    while (match(TokenType::KwAnd)) {
        ExprPtr right = parseEquality();
        left = std::make_unique<BinaryExpr>(BinaryOp::And, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseEquality() {
    ExprPtr left = parseRelational();
    while (check(TokenType::Eq) || check(TokenType::Ne)) {
        const BinaryOp op = check(TokenType::Eq) ? BinaryOp::Eq : BinaryOp::Ne;
        advance();
        ExprPtr right = parseRelational();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseRelational() {
    ExprPtr left = parseAdditive();
    while (check(TokenType::Lt) || check(TokenType::Gt) || check(TokenType::Le) ||
           check(TokenType::Ge)) {
        BinaryOp op;
        if (check(TokenType::Lt)) {
            op = BinaryOp::Lt;
        } else if (check(TokenType::Gt)) {
            op = BinaryOp::Gt;
        } else if (check(TokenType::Le)) {
            op = BinaryOp::Le;
        } else {
            op = BinaryOp::Ge;
        }
        advance();
        ExprPtr right = parseAdditive();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseAdditive() {
    ExprPtr left = parseMultiplicative();
    while (check(TokenType::Plus) || check(TokenType::Minus)) {
        const BinaryOp op = check(TokenType::Plus) ? BinaryOp::Add : BinaryOp::Sub;
        advance();
        ExprPtr right = parseMultiplicative();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseMultiplicative() {
    ExprPtr left = parseUnary();
    while (check(TokenType::Star) || check(TokenType::Slash)) {
        const BinaryOp op = check(TokenType::Star) ? BinaryOp::Mul : BinaryOp::Div;
        advance();
        ExprPtr right = parseUnary();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    return left;
}

ExprPtr Parser::parseUnary() {
    if (match(TokenType::KwNot)) {
        return std::make_unique<UnaryExpr>(UnaryOp::Not, parseUnary());
    }
    if (match(TokenType::Minus)) {
        return std::make_unique<UnaryExpr>(UnaryOp::Neg, parseUnary());
    }
    return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
    if (check(TokenType::IntLiteral)) {
        const Token& tok = advance();
        return std::make_unique<LiteralExpr>(static_cast<std::int32_t>(tok.intValue));
    }
    if (check(TokenType::RealLiteral)) {
        const Token& tok = advance();
        return std::make_unique<LiteralExpr>(tok.realValue);
    }
    if (check(TokenType::BoolLiteral)) {
        const Token& tok = advance();
        return std::make_unique<LiteralExpr>(tok.boolValue);
    }
    if (check(TokenType::TimeLiteral)) {
        const Token& tok = advance();
        return std::make_unique<LiteralExpr>(tok.timeValue);
    }
    if (check(TokenType::StringLiteral)) {
        const Token& tok = advance();
        return std::make_unique<LiteralExpr>(tok.stringValue);
    }
    if (check(TokenType::Identifier)) {
        const Token& tok = advance();
        return std::make_unique<IdentifierExpr>(tok.text);
    }
    if (match(TokenType::LParen)) {
        ExprPtr expr = parseExpression();
        expect(TokenType::RParen, "')'");
        return expr;
    }
    error("expected an expression");
}

}  // namespace softplc::st
