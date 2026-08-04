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

CompilationUnit parseUnit(const std::string& source) {
    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    return parser.parseCompilationUnit();
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
    ASSERT_TRUE(ast.varDecls[0].elementaryType.has_value());
    EXPECT_EQ(*ast.varDecls[0].elementaryType, softplc::tags::TypeId::Bool);
    EXPECT_EQ(ast.varDecls[1].name, "Y");
    ASSERT_TRUE(ast.varDecls[1].elementaryType.has_value());
    EXPECT_EQ(*ast.varDecls[1].elementaryType, softplc::tags::TypeId::DInt);
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

TEST(ParserTest, ParsesFunctionBlockDef) {
    auto unit = parseUnit(R"(
        FUNCTION_BLOCK FB_Edge
        VAR_INPUT
            CLK : BOOL;
        END_VAR
        VAR_OUTPUT
            Q : BOOL;
        END_VAR
        VAR
            M : BOOL;
        END_VAR
        Q := CLK AND NOT M;
        M := CLK;
        END_FUNCTION_BLOCK

        PROGRAM Test
        END_PROGRAM
    )");

    ASSERT_EQ(unit.pous.size(), 1u);
    const auto& fb = unit.pous[0];
    EXPECT_EQ(fb.name, "FB_Edge");
    EXPECT_FALSE(fb.isFunction);
    ASSERT_EQ(fb.varDecls.size(), 3u);
    EXPECT_EQ(fb.varDecls[0].kind, VarKind::VarInput);
    EXPECT_EQ(fb.varDecls[1].kind, VarKind::VarOutput);
    EXPECT_EQ(fb.varDecls[2].kind, VarKind::Var);
    ASSERT_EQ(fb.body.size(), 2u);
}

TEST(ParserTest, ParsesFunctionDefWithVarTemp) {
    auto unit = parseUnit(R"(
        FUNCTION FC_Clamp
        VAR_INPUT
            Value : DINT;
            Lo : DINT;
            Hi : DINT;
        END_VAR
        VAR_OUTPUT
            Result : DINT;
        END_VAR
        VAR_TEMP
            Scratch : DINT;
        END_VAR
        Scratch := Value;
        Result := Scratch;
        END_FUNCTION

        PROGRAM Test
        END_PROGRAM
    )");

    ASSERT_EQ(unit.pous.size(), 1u);
    const auto& fc = unit.pous[0];
    EXPECT_EQ(fc.name, "FC_Clamp");
    EXPECT_TRUE(fc.isFunction);
    ASSERT_EQ(fc.varDecls.size(), 5u);
    EXPECT_EQ(fc.varDecls[4].kind, VarKind::VarTemp);
}

TEST(ParserTest, RejectsPersistentVarInFunction) {
    EXPECT_THROW(parseUnit(R"(
        FUNCTION FC_Bad
        VAR
            X : DINT;
        END_VAR
        END_FUNCTION
        PROGRAM Test
        END_PROGRAM
    )"),
                 ParseError);
}

TEST(ParserTest, ParsesDataBlockDef) {
    auto unit = parseUnit(R"(
        DATA_BLOCK DB1
        VAR
            Speed : INT := 10;
            Running : BOOL;
        END_VAR
        END_DATA_BLOCK

        PROGRAM Test
        END_PROGRAM
    )");

    ASSERT_EQ(unit.dataBlocks.size(), 1u);
    const auto& db = unit.dataBlocks[0];
    EXPECT_EQ(db.name, "DB1");
    ASSERT_EQ(db.varDecls.size(), 2u);
    EXPECT_EQ(db.varDecls[0].name, "Speed");
    EXPECT_EQ(db.varDecls[1].name, "Running");
}

TEST(ParserTest, ParsesInstanceDeclaration) {
    auto unit = parseUnit(R"(
        FUNCTION_BLOCK FB_X
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
        END_VAR
        END_PROGRAM
    )");

    ASSERT_EQ(unit.program.varDecls.size(), 1u);
    const auto& decl = unit.program.varDecls[0];
    EXPECT_FALSE(decl.elementaryType.has_value());
    ASSERT_TRUE(decl.instanceOfType.has_value());
    EXPECT_EQ(*decl.instanceOfType, "FB_X");
}

TEST(ParserTest, RejectsInstanceDeclarationInVarInput) {
    EXPECT_THROW(parseUnit(R"(
        FUNCTION_BLOCK FB_X
        END_FUNCTION_BLOCK

        FUNCTION_BLOCK FB_Y
        VAR_INPUT
            Inst1 : FB_X;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        END_PROGRAM
    )"),
                 ParseError);
}

TEST(ParserTest, ParsesCallStatementWithMixedArgs) {
    auto unit = parseUnit(R"(
        FUNCTION_BLOCK FB_X
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        VAR_OUTPUT
            Out1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
            Local : BOOL;
        END_VAR
        Inst1(In1 := TRUE, Out1 => Local);
        END_PROGRAM
    )");

    ASSERT_EQ(unit.program.body.size(), 1u);
    EXPECT_EQ(unit.program.body[0]->kind, StmtKind::Call);
    const auto& call = static_cast<const CallStmt&>(*unit.program.body[0]);
    EXPECT_EQ(call.calleeName, "Inst1");
    ASSERT_EQ(call.args.size(), 2u);
    EXPECT_EQ(call.args[0].paramName, "In1");
    EXPECT_FALSE(call.args[0].isOutput);
    EXPECT_EQ(call.args[1].paramName, "Out1");
    EXPECT_TRUE(call.args[1].isOutput);
    EXPECT_EQ(call.args[1].outputTargetName, "Local");
}

TEST(ParserTest, ParsesDottedIdentifierInExpression) {
    auto unit = parseUnit(R"(
        DATA_BLOCK DB1
        VAR
            Speed : INT := 0;
        END_VAR
        END_DATA_BLOCK

        PROGRAM Test
        VAR
            X : INT := 0;
        END_VAR
        X := DB1.Speed;
        END_PROGRAM
    )");

    ASSERT_EQ(unit.program.body.size(), 1u);
    const auto& assign = static_cast<const AssignStmt&>(*unit.program.body[0]);
    ASSERT_EQ(assign.value->kind, ExprKind::Identifier);
    const auto& id = static_cast<const IdentifierExpr&>(*assign.value);
    EXPECT_EQ(id.name, "DB1.Speed");
}

TEST(ParserTest, ThrowsOnMissingArrowOrAssignInCallArg) {
    EXPECT_THROW(parseUnit(R"(
        FUNCTION_BLOCK FB_X
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
        END_VAR
        Inst1(In1 TRUE);
        END_PROGRAM
    )"),
                 ParseError);
}

TEST(ParserTest, ThrowsOnMultipleProgramsInOneUnit) {
    EXPECT_THROW(parseUnit(R"(
        PROGRAM A
        END_PROGRAM
        PROGRAM B
        END_PROGRAM
    )"),
                 ParseError);
}

TEST(ParserTest, ThrowsOnMissingProgram) {
    EXPECT_THROW(parseUnit(R"(
        FUNCTION_BLOCK FB_X
        END_FUNCTION_BLOCK
    )"),
                 ParseError);
}
