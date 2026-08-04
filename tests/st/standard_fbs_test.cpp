#include "softplc/st/standard_fbs.hpp"

#include <gtest/gtest.h>

#include "softplc/core/scan_engine.hpp"
#include "softplc/st/st_program.hpp"
#include "softplc/tags/tag_store.hpp"

using namespace softplc;

namespace {

// Drives one "scan" of a loaded StProgram deterministically: writes an explicit
// elapsed-time sample into the well-known System.CycleTime tag (exactly what
// ScanEngine::runOnce() does with the real measured wall-clock delta -- see
// docs/architecture.md's "Standard library: TON/TOF/CTU/CTD" section) before
// executing, bypassing real timing so TON/TOF tests are exact and fast rather than
// sleep-based and flaky.
void tick(core::IProgram& program, tags::TagStore& tags, tags::TimeValue dt) {
    tags.write(tags.find("System.CycleTime").value(), dt);
    core::ScanDiagnostics diagnostics;
    core::ScanContext ctx{
        .cycleTime = std::chrono::microseconds(0),
        .cycleStart = std::chrono::steady_clock::now(),
        .tags = tags,
        .diagnostics = diagnostics,
    };
    program.execute(ctx);
}

}  // namespace

TEST(StandardFbTest, TonRaisesQOnceElapsedTimeReachesPresetAndResetsWhenInputDrops) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Start : BOOL := FALSE;
            Timer1 : TON;
        END_VAR
        Timer1(IN := Start, PT := T#100ms);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto startId = tags.find("Start").value();
    auto qId = tags.find("Timer1.Q").value();
    auto etId = tags.find("Timer1.ET").value();

    tags.write(startId, true);

    tick(*program, tags, tags::TimeValue(40));
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));
    EXPECT_EQ(std::get<tags::TimeValue>(tags.read(etId)).count(), 40);

    tick(*program, tags, tags::TimeValue(40));
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));
    EXPECT_EQ(std::get<tags::TimeValue>(tags.read(etId)).count(), 80);

    // 80 + 40 = 120ms would overshoot PT=100ms: ET clamps at PT and Q raises.
    tick(*program, tags, tags::TimeValue(40));
    EXPECT_TRUE(std::get<bool>(tags.read(qId)));
    EXPECT_EQ(std::get<tags::TimeValue>(tags.read(etId)).count(), 100);

    // IN drops before the next scan: ET resets to zero and Q drops immediately.
    tags.write(startId, false);
    tick(*program, tags, tags::TimeValue(10));
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));
    EXPECT_EQ(std::get<tags::TimeValue>(tags.read(etId)).count(), 0);
}

TEST(StandardFbTest, TofHoldsQTrueUntilElapsedOffDelayReachesPreset) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Start : BOOL := FALSE;
            Timer1 : TOF;
        END_VAR
        Timer1(IN := Start, PT := T#100ms);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto startId = tags.find("Start").value();
    auto qId = tags.find("Timer1.Q").value();

    // IN true: Q rises immediately, with no delay.
    tags.write(startId, true);
    tick(*program, tags, tags::TimeValue(5));
    EXPECT_TRUE(std::get<bool>(tags.read(qId)));

    // IN drops: Q stays TRUE while ET accumulates toward PT.
    tags.write(startId, false);
    tick(*program, tags, tags::TimeValue(60));
    EXPECT_TRUE(std::get<bool>(tags.read(qId)));

    tick(*program, tags, tags::TimeValue(60));  // 60 + 60 = 120ms >= PT=100ms
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));
}

TEST(StandardFbTest, CtuCountsRisingEdgesAndResetsOnR) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Pulse : BOOL := FALSE;
            Reset1 : BOOL := FALSE;
            Counter1 : CTU;
        END_VAR
        Counter1(CU := Pulse, R := Reset1, PV := 3);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto pulseId = tags.find("Pulse").value();
    auto resetId = tags.find("Reset1").value();
    auto cvId = tags.find("Counter1.CV").value();
    auto qId = tags.find("Counter1.Q").value();

    tags.write(pulseId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 1);
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));

    // CU stays high (no new rising edge): CV must not double-count.
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 1);

    tags.write(pulseId, false);
    tick(*program, tags, tags::TimeValue(0));
    tags.write(pulseId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 2);

    tags.write(pulseId, false);
    tick(*program, tags, tags::TimeValue(0));
    tags.write(pulseId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 3);
    EXPECT_TRUE(std::get<bool>(tags.read(qId)));

    tags.write(resetId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 0);
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));
}

TEST(StandardFbTest, CtdCountsDownFromLoadedPresetAndSignalsAtZero) {
    const std::string source = R"(
        PROGRAM Test
        VAR
            Pulse : BOOL := FALSE;
            Load1 : BOOL := FALSE;
            Counter1 : CTD;
        END_VAR
        Counter1(CD := Pulse, LD := Load1, PV := 2);
        END_PROGRAM
    )";

    tags::TagStore tags;
    auto program = st::StProgram::load(source, tags);
    auto pulseId = tags.find("Pulse").value();
    auto loadId = tags.find("Load1").value();
    auto cvId = tags.find("Counter1.CV").value();
    auto qId = tags.find("Counter1.Q").value();

    tags.write(loadId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 2);
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));

    tags.write(loadId, false);
    tags.write(pulseId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 1);
    EXPECT_FALSE(std::get<bool>(tags.read(qId)));

    // CD stays high (no new edge): CV must not double-decrement.
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 1);

    tags.write(pulseId, false);
    tick(*program, tags, tags::TimeValue(0));
    tags.write(pulseId, true);
    tick(*program, tags, tags::TimeValue(0));
    EXPECT_EQ(std::get<std::int32_t>(tags.read(cvId)), 0);
    EXPECT_TRUE(std::get<bool>(tags.read(qId)));
}

TEST(StandardFbTest, UserDefinedPouWithReservedStandardFbNameThrowsAtLoadTime) {
    const std::string source = R"(
        FUNCTION_BLOCK TON
        END_FUNCTION_BLOCK

        PROGRAM Test
        END_PROGRAM
    )";
    tags::TagStore tags;
    EXPECT_THROW(st::StProgram::load(source, tags), std::runtime_error);
}
