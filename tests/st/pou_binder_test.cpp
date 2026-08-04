#include "softplc/st/pou_binder.hpp"

#include <gtest/gtest.h>

#include "softplc/st/interpreter.hpp"
#include "softplc/st/lexer.hpp"
#include "softplc/st/parser.hpp"

using namespace softplc;
using namespace softplc::st;

namespace {

StProgramAst bindSource(const std::string& source, tags::TagStore& tags) {
    Lexer lexer(source);
    Parser parser(lexer.tokenize());
    CompilationUnit unit = parser.parseCompilationUnit();
    return bindCompilationUnit(std::move(unit), tags);
}

}  // namespace

TEST(PouBinderTest, IndependentFbInstancesHaveIsolatedState) {
    const std::string source = R"(
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
        VAR
            M1 : FB_Edge;
            M2 : FB_Edge;
        END_VAR
        M1(CLK := TRUE);
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto m1Q = tags.find("M1.Q");
    const auto m2Q = tags.find("M2.Q");
    const auto m1M = tags.find("M1.M");
    const auto m2M = tags.find("M2.M");
    ASSERT_TRUE(m1Q.has_value());
    ASSERT_TRUE(m2Q.has_value());
    ASSERT_TRUE(m1M.has_value());
    ASSERT_TRUE(m2M.has_value());

    EXPECT_TRUE(std::get<bool>(tags.read(*m1Q)));
    EXPECT_TRUE(std::get<bool>(tags.read(*m1M)));
    // M2 was never called: its state must be completely untouched by M1's execution.
    EXPECT_FALSE(std::get<bool>(tags.read(*m2Q)));
    EXPECT_FALSE(std::get<bool>(tags.read(*m2M)));
}

TEST(PouBinderTest, NestedInstanceOfInstanceIsIndependentPerOuterInstance) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_Inner
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        VAR_OUTPUT
            Out1 : BOOL;
        END_VAR
        Out1 := In1;
        END_FUNCTION_BLOCK

        FUNCTION_BLOCK FB_Outer
        VAR_INPUT
            X : BOOL;
        END_VAR
        VAR
            Inner : FB_Inner;
        END_VAR
        Inner(In1 := X);
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Outer1 : FB_Outer;
            Outer2 : FB_Outer;
        END_VAR
        Outer1(X := TRUE);
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto outer1InnerOut = tags.find("Outer1.Inner.Out1");
    const auto outer2InnerOut = tags.find("Outer2.Inner.Out1");
    ASSERT_TRUE(outer1InnerOut.has_value());
    ASSERT_TRUE(outer2InnerOut.has_value());

    EXPECT_TRUE(std::get<bool>(tags.read(*outer1InnerOut)));
    // Outer2 was never called, so its nested Inner instance is untouched too --
    // proves four independently-addressable, independently-stateful tag groups.
    EXPECT_FALSE(std::get<bool>(tags.read(*outer2InnerOut)));
}

TEST(PouBinderTest, TwoFunctionCallSitesDoNotLeakStateBetweenEachOther) {
    const std::string source = R"(
        FUNCTION FC_Double
        VAR_INPUT
            In1 : DINT;
        END_VAR
        VAR_OUTPUT
            Out1 : DINT;
        END_VAR
        VAR_TEMP
            Scratch : DINT;
        END_VAR
        Scratch := In1 * 2;
        Out1 := Scratch;
        END_FUNCTION

        PROGRAM Test
        VAR
            A : DINT := 3;
            B : DINT := 10;
            ResultA : DINT;
            ResultB : DINT;
        END_VAR
        FC_Double(In1 := A, Out1 => ResultA);
        FC_Double(In1 := B, Out1 => ResultB);
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto resultA = tags.find("ResultA").value();
    const auto resultB = tags.find("ResultB").value();
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultA)), 6);
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultB)), 20);
}

TEST(PouBinderTest, VarTempResetsBeforeEachFrameExecution) {
    const std::string source = R"(
        FUNCTION FC_Echo
        VAR_INPUT
            In1 : DINT;
        END_VAR
        VAR_OUTPUT
            Out1 : DINT;
        END_VAR
        VAR_TEMP
            Scratch : DINT;
        END_VAR
        Out1 := Scratch;
        Scratch := In1;
        END_FUNCTION

        PROGRAM Test
        VAR
            A : DINT := 42;
            ResultA : DINT;
        END_VAR
        FC_Echo(In1 := A, Out1 => ResultA);
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    const auto resultId = tags.find("ResultA").value();

    interp.run(tags);
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultId)), 0);

    // If VAR_TEMP weren't reset before this second execution, Scratch would still
    // hold 42 from the first run's "Scratch := In1;", and Out1 would read 42 here.
    interp.run(tags);
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultId)), 0);
}

TEST(PouBinderTest, ThrowsOnUndeclaredInstanceType) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Inst1 : FB_DoesNotExist;
        END_VAR
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, ThrowsOnUnknownCallArgParameterName) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_X
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
        END_VAR
        Inst1(Bogus := TRUE);
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, ThrowsOnAssignToOutputOnlyParam) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_X
        VAR_OUTPUT
            Out1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
        END_VAR
        Inst1(Out1 := TRUE);
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, ThrowsOnArrowBindingToInputOnlyParam) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_X
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
            Local : BOOL;
        END_VAR
        Inst1(In1 => Local);
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, ThrowsOnOmittedRequiredFunctionInput) {
    const std::string source = R"(
        FUNCTION FC_NeedsInput
        VAR_INPUT
            In1 : BOOL;
        END_VAR
        VAR_OUTPUT
            Out1 : BOOL;
        END_VAR
        Out1 := In1;
        END_FUNCTION

        PROGRAM Test
        VAR
            Result : BOOL;
        END_VAR
        FC_NeedsInput(Out1 => Result);
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, ThrowsOnCircularFunctionBlockInstantiation) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_A
        VAR
            B1 : FB_B;
        END_VAR
        END_FUNCTION_BLOCK

        FUNCTION_BLOCK FB_B
        VAR
            A1 : FB_A;
        END_VAR
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_A;
        END_VAR
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, DataBlockReadableFromProgramAndFromInsideFbBody) {
    const std::string source = R"(
        DATA_BLOCK DB1
        VAR
            Speed : INT := 100;
        END_VAR
        END_DATA_BLOCK

        FUNCTION_BLOCK FB_Reader
        VAR_OUTPUT
            Copy : INT;
        END_VAR
        Copy := DB1.Speed;
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            R1 : FB_Reader;
            Local : INT;
        END_VAR
        R1();
        Local := DB1.Speed;
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto copyId = tags.find("R1.Copy").value();
    const auto localId = tags.find("Local").value();
    EXPECT_EQ(std::get<std::int16_t>(tags.read(copyId)), 100);
    EXPECT_EQ(std::get<std::int16_t>(tags.read(localId)), 100);
}

TEST(PouBinderTest, ProgramCanWriteDataBlockMember) {
    const std::string source = R"(
        DATA_BLOCK DB1
        VAR
            Speed : INT := 100;
        END_VAR
        END_DATA_BLOCK

        PROGRAM Test
        DB1.Speed := 200;
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto speedId = tags.find("DB1.Speed").value();
    EXPECT_EQ(std::get<std::int16_t>(tags.read(speedId)), 200);
}

TEST(PouBinderTest, ThrowsWhenRungOutputTargetIsNotBool) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Start : BOOL := FALSE;
            Count : DINT := 0;
        END_VAR
        RUNG Start => Count;
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(bindSource(source, tags), std::runtime_error);
}

TEST(PouBinderTest, FbOutputReadableByDottedNameWithoutArrowBinding) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_X
        VAR_OUTPUT
            Out1 : BOOL;
        END_VAR
        Out1 := TRUE;
        END_FUNCTION_BLOCK

        PROGRAM Test
        VAR
            Inst1 : FB_X;
        END_VAR
        Inst1();
        END_PROGRAM
    )";

    tags::TagStore tags;
    StProgramAst ast = bindSource(source, tags);
    Interpreter interp(ast);
    interp.run(tags);

    const auto outId = tags.find("Inst1.Out1").value();
    EXPECT_TRUE(std::get<bool>(tags.read(outId)));
}
