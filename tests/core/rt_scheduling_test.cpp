#include "softplc/core/rt_scheduling.hpp"

#include <gtest/gtest.h>

#include <thread>

using namespace softplc::core::rt;

TEST(RtSchedulingTest, DefaultPolicyIsANoOp) {
    RtApplyResult result = applyRealtimePolicy(RtPolicy{});
    EXPECT_FALSE(result.prioritySet);
    EXPECT_FALSE(result.affinitySet);
    EXPECT_FALSE(result.memoryLocked);
    EXPECT_TRUE(result.warnings.empty());
}

TEST(RtSchedulingTest, RealtimePriorityNeverThrowsAndReportsOutcome) {
    RtPolicy policy;
    policy.enableRealtimePriority = true;
    policy.priority = 50;

    RtApplyResult result;
    EXPECT_NO_THROW(result = applyRealtimePolicy(policy));

    // This sandbox is expected to be unprivileged (no CAP_SYS_NICE/root), so
    // prioritySet should be false with an explanatory warning — but tolerate a
    // privileged CI runner too, without hardcoding which branch fires.
    if (!result.prioritySet) {
        EXPECT_FALSE(result.warnings.empty());
    }
}

TEST(RtSchedulingTest, CpuAffinityToOwnThreadUsuallySucceeds) {
    const unsigned available = std::thread::hardware_concurrency();
    if (available == 0) {
        GTEST_SKIP() << "hardware_concurrency() is unknown on this platform";
    }

    RtPolicy policy;
    policy.cpuAffinity = available - 1;

    RtApplyResult result;
    EXPECT_NO_THROW(result = applyRealtimePolicy(policy));

    // Setting your own thread's affinity needs no special privilege on Linux, so
    // this should reliably succeed in this sandbox; still don't hard-fail if some
    // exotic environment disallows it, as long as a warning explains why.
    if (!result.affinitySet) {
        EXPECT_FALSE(result.warnings.empty());
    }
}

TEST(RtSchedulingTest, OutOfRangeCpuAffinityWarnsInsteadOfCrashing) {
    RtPolicy policy;
    policy.cpuAffinity = std::thread::hardware_concurrency() + 1000;

    RtApplyResult result;
    EXPECT_NO_THROW(result = applyRealtimePolicy(policy));

    EXPECT_FALSE(result.affinitySet);
    EXPECT_FALSE(result.warnings.empty());
}

TEST(RtSchedulingTest, LockMemoryNeverThrowsAndReportsOutcome) {
    RtPolicy policy;
    policy.lockMemory = true;

    RtApplyResult result;
    EXPECT_NO_THROW(result = applyRealtimePolicy(policy));

    // Expected to fail here (default RLIMIT_MEMLOCK in an unprivileged sandbox, or
    // simply unsupported on this platform), but must degrade to a warning.
    if (!result.memoryLocked) {
        EXPECT_FALSE(result.warnings.empty());
    }
}

TEST(RtSchedulingTest, PrefaultStackDoesNotCrash) {
    EXPECT_NO_THROW(prefaultStack());
    EXPECT_NO_THROW(prefaultStack(0));
    EXPECT_NO_THROW(prefaultStack(1024 * 1024 * 1024));  // clamped internally, must not overflow
}
