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
