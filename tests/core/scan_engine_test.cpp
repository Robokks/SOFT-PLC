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
