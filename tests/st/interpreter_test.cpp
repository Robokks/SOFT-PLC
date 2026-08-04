#include "softplc/st/st_program.hpp"

#include <gtest/gtest.h>

#include "softplc/core/scan_engine.hpp"
#include "softplc/io/simulated_io_driver.hpp"
#include "softplc/tags/tag_store.hpp"

using namespace softplc;

TEST(InterpreterTest, SimpleAssignmentAndArithmetic) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        X := 2 + 3 * 4;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto id = tags.find("X").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();

    EXPECT_EQ(std::get<std::int32_t>(tags.read(id)), 14);
}

TEST(InterpreterTest, IfElsifElseSelectsCorrectBranch) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            X : DINT := 5;
            Result : DINT := 0;
        END_VAR
        IF X = 0 THEN
            Result := 100;
        ELSIF X = 5 THEN
            Result := 200;
        ELSE
            Result := 300;
        END_IF;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto resultId = tags.find("Result").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();

    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultId)), 200);
}

TEST(InterpreterTest, WhileLoopComputesSum) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            I : DINT := 0;
            Sum : DINT := 0;
        END_VAR
        WHILE I < 10 DO
            Sum := Sum + I;
            I := I + 1;
        END_WHILE;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto sumId = tags.find("Sum").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();

    EXPECT_EQ(std::get<std::int32_t>(tags.read(sumId)), 45);
}

TEST(InterpreterTest, TimeLiteralParsesToCorrectMilliseconds) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            T1 : TIME := T#1h30m;
        END_VAR
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto id = tags.find("T1").value();

    EXPECT_EQ(std::get<tags::TimeValue>(tags.read(id)).count(), 90 * 60 * 1000);
}

TEST(InterpreterTest, BlinkExampleTogglesOutputAfterThreshold) {
    const std::string source = R"(
        PROGRAM Blink
        VAR
            Counter : DINT := 0;
            ToggleAfterScans : DINT := 3;
            LedState AT %Q0.0 : BOOL := FALSE;
        END_VAR
        Counter := Counter + 1;
        IF Counter >= ToggleAfterScans THEN
            LedState := NOT LedState;
            Counter := 0;
        END_IF;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto ledId = tags.find("LedState").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    EXPECT_FALSE(std::get<bool>(tags.read(ledId)));
    engine.runOnce();
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(ledId)));
    engine.runOnce();  // third scan crosses the threshold
    EXPECT_TRUE(std::get<bool>(tags.read(ledId)));
}

TEST(InterpreterTest, EdgeDetectFbTriggersOnRisingEdgeAcrossScans) {
    const std::string source = R"(
        FUNCTION_BLOCK FB_REdge
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
            Trigger : BOOL := FALSE;
            Edge1 : FB_REdge;
        END_VAR
        Edge1(CLK := Trigger);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto triggerId = tags.find("Trigger").value();
    auto edgeQId = tags.find("Edge1.Q").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    // Scan 1: Trigger stays false -- no edge.
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(edgeQId)));

    // Scan 2: Trigger rises to true -- Q pulses true this scan.
    tags.write(triggerId, true);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(edgeQId)));

    // Scan 3: Trigger stays true (already high) -- not a new edge, Q drops back.
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(edgeQId)));

    // Scan 4/5: Trigger falls then rises again -- a second pulse.
    tags.write(triggerId, false);
    engine.runOnce();
    tags.write(triggerId, true);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(edgeQId)));
}

TEST(InterpreterTest, FunctionCallDoesNotLeakStateBetweenScans) {
    const std::string source = R"(
        FUNCTION FC_Square
        VAR_INPUT
            In1 : DINT;
        END_VAR
        VAR_OUTPUT
            Out1 : DINT;
        END_VAR
        Out1 := In1 * In1;
        END_FUNCTION

        PROGRAM Test
        VAR
            N : DINT := 0;
            Result : DINT := 0;
        END_VAR
        FC_Square(In1 := N, Out1 => Result);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto nId = tags.find("N").value();
    auto resultId = tags.find("Result").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    tags.write(nId, std::int32_t{3});
    engine.runOnce();
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultId)), 9);

    tags.write(nId, std::int32_t{5});
    engine.runOnce();
    EXPECT_EQ(std::get<std::int32_t>(tags.read(resultId)), 25);
}

TEST(InterpreterTest, MultiPouIntegrationDbFbAndFcTogether) {
    const std::string source = R"(
        DATA_BLOCK DB1
        VAR
            Setpoint : DINT := 50;
        END_VAR
        END_DATA_BLOCK

        FUNCTION_BLOCK FB_Limiter
        VAR_INPUT
            In1 : DINT;
        END_VAR
        VAR_OUTPUT
            Out1 : DINT;
        END_VAR
        IF In1 > DB1.Setpoint THEN
            Out1 := DB1.Setpoint;
        ELSE
            Out1 := In1;
        END_IF;
        END_FUNCTION_BLOCK

        FUNCTION FC_Double
        VAR_INPUT
            In1 : DINT;
        END_VAR
        VAR_OUTPUT
            Out1 : DINT;
        END_VAR
        Out1 := In1 * 2;
        END_FUNCTION

        PROGRAM Test
        VAR
            Raw : DINT := 40;
            Limited : DINT;
            Doubled : DINT;
            Limiter1 : FB_Limiter;
        END_VAR
        Limiter1(In1 := Raw, Out1 => Limited);
        FC_Double(In1 := Limited, Out1 => Doubled);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto limitedId = tags.find("Limited").value();
    auto doubledId = tags.find("Doubled").value();
    auto rawId = tags.find("Raw").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    // Raw (40) is under DB1.Setpoint (50): passes through unchanged, then doubled.
    engine.runOnce();
    EXPECT_EQ(std::get<std::int32_t>(tags.read(limitedId)), 40);
    EXPECT_EQ(std::get<std::int32_t>(tags.read(doubledId)), 80);

    // Raise Raw above the setpoint: the limiter clamps it before doubling.
    tags.write(rawId, std::int32_t{999});
    engine.runOnce();
    EXPECT_EQ(std::get<std::int32_t>(tags.read(limitedId)), 50);
    EXPECT_EQ(std::get<std::int32_t>(tags.read(doubledId)), 100);
}

TEST(InterpreterTest, UndeclaredIdentifierThrowsAtLoadTime) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
        END_VAR
        Y := 1;
        END_PROGRAM
    )";

    tags::TagStore tags;
    EXPECT_THROW(st::StProgram::load(source, tags), std::runtime_error);
}

TEST(InterpreterTest, IntegerLiteralAssignedToNonDIntTargetCoercesToDeclaredType) {
    // Bare integer literals are always parsed as DINT (see Parser::parsePrimary);
    // the interpreter must narrow to X's actual declared type (INT) on assignment
    // rather than leaving a DInt-typed Value in a Value variant declared as Int.
    const std::string source = R"(
        PROGRAM Test
        VAR
            X : INT := 0;
        END_VAR
        X := 200;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto id = tags.find("X").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();

    EXPECT_EQ(std::get<std::int16_t>(tags.read(id)), 200);
}

TEST(InterpreterTest, LadderSealInCircuitLatchesViaFeedbackContact) {
    // Classic motor seal-in rung: "(Start OR Motor) AND NOT Stop => Motor;" -- Motor's
    // own coil is read back as a contact on the same rung, a standard ladder idiom
    // (series = AND, parallel = OR, feedback = reading the coil's own tag).
    const std::string source = R"(
        PROGRAM Test
        VAR
            Start : BOOL := FALSE;
            Stop : BOOL := FALSE;
            Motor : BOOL := FALSE;
        END_VAR
        RUNG (Start OR Motor) AND NOT Stop => Motor;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto startId = tags.find("Start").value();
    auto stopId = tags.find("Stop").value();
    auto motorId = tags.find("Motor").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    // Start pulses true for one scan: Motor latches on.
    tags.write(startId, true);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(motorId)));

    // Start drops back to false: Motor stays on via its own feedback contact.
    tags.write(startId, false);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(motorId)));

    // Stop breaks the seal-in: Motor drops out.
    tags.write(stopId, true);
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(motorId)));

    // Stop releases: Motor stays off (nothing is driving Start OR Motor anymore).
    tags.write(stopId, false);
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(motorId)));
}

TEST(InterpreterTest, LadderSetResetCoilsLatchWithoutFeedbackContact) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Start : BOOL := FALSE;
            Stop : BOOL := FALSE;
            Motor : BOOL := FALSE;
        END_VAR
        RUNG Start => SET Motor;
        RUNG Stop => RESET Motor;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto startId = tags.find("Start").value();
    auto stopId = tags.find("Stop").value();
    auto motorId = tags.find("Motor").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    tags.write(startId, true);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(motorId)));

    // Neither Start nor Stop is powered: the SET coil must NOT re-drive Motor to
    // FALSE just because its rung isn't powered this scan (unlike a Direct coil).
    tags.write(startId, false);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(motorId)));

    tags.write(stopId, true);
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(motorId)));

    tags.write(stopId, false);
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(motorId)));
}

TEST(InterpreterTest, RungReadsFbInstanceOutputPlacedAdjacentAsABox) {
    // A future GUI would place Edge1 "on" the rung as a box; v1's textual grammar
    // achieves the same effect by placing the CallStmt immediately before the RUNG
    // that reads its Q output like any other contact.
    const std::string source = R"(
        FUNCTION_BLOCK FB_REdge
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
            Trigger : BOOL := FALSE;
            Edge1 : FB_REdge;
            Pulse : BOOL := FALSE;
        END_VAR
        Edge1(CLK := Trigger);
        RUNG Edge1.Q => Pulse;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto triggerId = tags.find("Trigger").value();
    auto pulseId = tags.find("Pulse").value();

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));

    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(pulseId)));

    // Trigger rises: FB_REdge pulses Q true this scan, and the RUNG's contact on
    // Edge1.Q drives Pulse true in the same scan.
    tags.write(triggerId, true);
    engine.runOnce();
    EXPECT_TRUE(std::get<bool>(tags.read(pulseId)));

    // Trigger stays high (no new edge): Q drops back, so does Pulse.
    engine.runOnce();
    EXPECT_FALSE(std::get<bool>(tags.read(pulseId)));
}

TEST(InterpreterTest, AssigningIncompatibleTypeThrowsAtRuntime) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            X : DINT := 0;
            Flag : BOOL := TRUE;
        END_VAR
        X := Flag;
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);

    io::SimulatedIoDriver io;
    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    EXPECT_THROW(engine.runOnce(), std::runtime_error);
}
