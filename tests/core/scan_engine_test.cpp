#include "softplc/core/scan_engine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "softplc/core/native_program.hpp"
#include "softplc/io/io_driver.hpp"
#include "softplc/tags/tag_store.hpp"

using namespace softplc;

namespace {

class RecordingIoDriver : public io::IIoDriver {
public:
    void readInputs(tags::TagStore&) override { events.push_back("read"); }
    void writeOutputs(const tags::TagStore&) override { events.push_back("write"); }
    [[nodiscard]] std::string_view name() const override { return "RecordingIoDriver"; }

    std::vector<std::string> events;
};

}  // namespace

TEST(ScanEngineTest, RunOnceCallsReadExecuteWriteInOrder) {
    tags::TagStore tags;
    RecordingIoDriver io;

    auto program = std::make_shared<core::NativeProgram>(
        "recorder", [&io](core::ScanContext&) { io.events.push_back("execute"); });

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();

    ASSERT_EQ(io.events.size(), 3u);
    EXPECT_EQ(io.events[0], "read");
    EXPECT_EQ(io.events[1], "execute");
    EXPECT_EQ(io.events[2], "write");
}

TEST(ScanEngineTest, RunOnceUpdatesDiagnosticsCycleCount) {
    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("noop", [](core::ScanContext&) {});

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    engine.runOnce();
    engine.runOnce();
    engine.runOnce();

    EXPECT_EQ(engine.diagnostics().cycleCount, 3u);
}

TEST(ScanEngineTest, OverrunIsDetectedWhenProgramExceedsCycleTime) {
    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("slow", [](core::ScanContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(5));
    engine.runOnce();

    EXPECT_EQ(engine.diagnostics().overrunCount, 1u);
}

TEST(ScanEngineTest, StartStopRunsBackgroundLoopForSeveralCycles) {
    tags::TagStore tags;
    RecordingIoDriver io;
    std::atomic<int> executeCount{0};
    auto program = std::make_shared<core::NativeProgram>(
        "counter", [&executeCount](core::ScanContext&) { ++executeCount; });

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(5));
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine.stop();

    EXPECT_GT(executeCount.load(), 0);
    EXPECT_FALSE(engine.isRunning());
}

TEST(ScanEngineTest, RunOnceUpdatesMinCycleDuration) {
    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("noop", [](core::ScanContext&) {});

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(10));
    EXPECT_EQ(engine.diagnostics().minCycleDuration, std::chrono::microseconds::max());

    engine.runOnce();

    EXPECT_LT(engine.diagnostics().minCycleDuration, std::chrono::microseconds::max());
}

TEST(ScanEngineTest, DefaultRtPolicyIsANoOpAfterStart) {
    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("noop", [](core::ScanContext&) {});

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(5));
    engine.start();
    engine.stop();

    EXPECT_FALSE(engine.rtApplyResult().prioritySet);
    EXPECT_FALSE(engine.rtApplyResult().affinitySet);
    EXPECT_FALSE(engine.rtApplyResult().memoryLocked);
    EXPECT_TRUE(engine.rtApplyResult().warnings.empty());
}

TEST(ScanEngineTest, StartAppliesRtPolicySynchronouslyBeforeReturning) {
    const unsigned available = std::thread::hardware_concurrency();
    if (available == 0) {
        GTEST_SKIP() << "hardware_concurrency() is unknown on this platform";
    }

    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("noop", [](core::ScanContext&) {});

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(5));
    core::rt::RtPolicy policy;
    policy.cpuAffinity = available - 1;
    engine.setRtPolicy(policy);

    engine.start();
    // start() blocks until the scan thread has applied the policy to itself, so
    // this must already be populated (race-free) by the time start() returns.
    const bool affinityOutcomeKnown =
        engine.rtApplyResult().affinitySet || !engine.rtApplyResult().warnings.empty();
    engine.stop();

    EXPECT_TRUE(affinityOutcomeKnown);
}

TEST(ScanEngineTest, FallingBehindScheduleIncrementsResyncCount) {
    tags::TagStore tags;
    RecordingIoDriver io;
    auto program = std::make_shared<core::NativeProgram>("slow", [](core::ScanContext&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    });

    core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(5));
    engine.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    engine.stop();

    // Each resync deliberately pushes nextTick into the future (see run()'s resync
    // branch), so wake jitter in this always-behind scenario tends to read as
    // negative/near-zero rather than positive — resyncCount, not maxWakeJitter, is
    // the meaningful signal here. Positive jitter reflects genuine OS scheduling
    // delay before a sleep_until() wake, which this test doesn't exercise.
    EXPECT_GE(engine.diagnostics().resyncCount, 1u);
}
