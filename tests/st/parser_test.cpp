#include "softplc/st/parser.hpp"

#include <gtest/gtest.h>

#include "softplc/st/lexer.hpp"

using namespace softplc::st;

namespace {

StProgramAst parse(const std::string& source) {
    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    return parser.parseProgram();
}

}  // namespace

TEST(ParserTest, ParsesProgramNameAndVarDecls) {
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            X : BOOL := TRUE;
            Y : DINT := 5;
        END_VAR
        END_PROGRAM
    )");

    EXPECT_EQ(ast.name, "Test");
    ASSERT_EQ(ast.varDecls.size(), 2u);
    EXPECT_EQ(ast.varDecls[0].name, "X");
    EXPECT_EQ(ast.varDecls[0].type, softplc::tags::TypeId::Bool);
    EXPECT_EQ(ast.varDecls[1].name, "Y");
    EXPECT_EQ(ast.varDecls[1].type, softplc::tags::TypeId::DInt);
}

TEST(ParserTest, ParsesDirectAddressWithAt) {
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            Led AT %Q0.0 : BOOL := FALSE;
        END_VAR
        END_PROGRAM
    )");

    ASSERT_EQ(ast.varDecls.size(), 1u);
    ASSERT_TRUE(ast.varDecls[0].address.has_value());
    EXPECT_EQ(ast.varDecls[0].address->area, softplc::tags::MemoryArea::Output);
}

TEST(ParserTest, ParsesAssignmentStatement) {
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        X := 1 + 2 * 3;
        END_PROGRAM
    )");

    ASSERT_EQ(ast.body.size(), 1u);
    EXPECT_EQ(ast.body[0]->kind, StmtKind::Assign);
    const auto& assign = static_cast<const AssignStmt&>(*ast.body[0]);
    EXPECT_EQ(assign.target, "X");
    ASSERT_EQ(assign.value->kind, ExprKind::Binary);
}

TEST(ParserTest, ParsesIfElsifElse) {
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        IF X = 0 THEN
            X := 1;
        ELSIF X = 1 THEN
            X := 2;
        ELSE
            X := 3;
        END_IF;
        END_PROGRAM
    )");

    ASSERT_EQ(ast.body.size(), 1u);
    const auto& ifStmt = static_cast<const IfStmt&>(*ast.body[0]);
    ASSERT_EQ(ifStmt.branches.size(), 2u);
    EXPECT_EQ(ifStmt.elseBody.size(), 1u);
}

TEST(ParserTest, ParsesWhileLoop) {
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        WHILE X < 10 DO
            X := X + 1;
        END_WHILE;
        END_PROGRAM
    )");

    ASSERT_EQ(ast.body.size(), 1u);
    EXPECT_EQ(ast.body[0]->kind, StmtKind::While);
}

TEST(ParserTest, ThrowsOnMissingEndVar) {
    EXPECT_THROW(parse("PROGRAM Test VAR X : BOOL; END_PROGRAM"), ParseError);
}

TEST(ParserTest, RespectsOperatorPrecedence) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3), i.e. top-level op is Add.
    auto ast = parse(R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        X := 1 + 2 * 3;
        END_PROGRAM
    )");
    const auto& assign = static_cast<const AssignStmt&>(*ast.body[0]);
    const auto& top = static_cast<const BinaryExpr&>(*assign.value);
    EXPECT_EQ(top.op, BinaryOp::Add);
    EXPECT_EQ(top.rhs->kind, ExprKind::Binary);
    const auto& rhs = static_cast<const BinaryExpr&>(*top.rhs);
    EXPECT_EQ(rhs.op, BinaryOp::Mul);
}
