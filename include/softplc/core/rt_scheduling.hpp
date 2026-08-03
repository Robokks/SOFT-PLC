#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace softplc::core::rt {

// Opt-in OS-level real-time scheduling for the calling thread. Everything here is
// best-effort: a missing capability (no CAP_SYS_NICE/root on Linux, a container's
// default RLIMIT_MEMLOCK) must degrade to a warning, never a crash, since most
// deployments (including CI) won't have these privileges.
struct RtPolicy {
    // SCHED_FIFO on Linux, THREAD_PRIORITY_TIME_CRITICAL on Windows.
    bool enableRealtimePriority = false;
    // Requested SCHED_FIFO priority on Linux; clamped into the OS-reported valid
    // range. Ignored (mapped to a fixed level) on Windows.
    int priority = 50;
    // Pin the thread to this logical CPU, if set.
    std::optional<unsigned> cpuAffinity;
    // mlockall(MCL_CURRENT | MCL_FUTURE) on Linux. No full-process equivalent exists
    // on Windows, so this is a documented no-op there (recorded as a warning).
    bool lockMemory = false;
};

struct RtApplyResult {
    bool prioritySet = false;
    bool affinitySet = false;
    bool memoryLocked = false;
    std::vector<std::string> warnings;
};

// Applies `policy` to the CALLING thread — must be invoked from the thread whose
// scheduling should change, not from whoever constructs a policy for it. Never
// throws; every failure is reported via RtApplyResult::warnings instead.
[[nodiscard]] RtApplyResult applyRealtimePolicy(const RtPolicy& policy);

// Touches up to `bytes` of the calling thread's own stack (clamped to a small,
// alloca-free, overflow-safe ceiling — this is a best-effort latency optimization,
// not a full-stack-depth guarantee) so that, once memory is locked, an early stack
// page fault doesn't itself introduce a latency spike later. Only worth calling
// when RtPolicy::lockMemory is set.
void prefaultStack(std::size_t bytes = 256 * 1024);

}  // namespace softplc::core::rt
