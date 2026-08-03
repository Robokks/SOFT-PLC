#include "softplc/core/rt_scheduling.hpp"

#include <algorithm>
#include <cstring>
#include <thread>

#if defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <cerrno>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace softplc::core::rt {

namespace {
void addWarning(RtApplyResult& result, std::string message) {
    result.warnings.push_back(std::move(message));
}
}  // namespace

#if defined(__linux__)

RtApplyResult applyRealtimePolicy(const RtPolicy& policy) {
    RtApplyResult result;

    if (policy.enableRealtimePriority) {
        const int minPriority = sched_get_priority_min(SCHED_FIFO);
        const int maxPriority = sched_get_priority_max(SCHED_FIFO);
        int requested = policy.priority;
        if (minPriority >= 0 && maxPriority >= 0) {
            requested = std::clamp(requested, minPriority, maxPriority);
        }

        sched_param param{};
        param.sched_priority = requested;
        // pthread_* functions return their error code directly; they do not set errno.
        const int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        if (rc == 0) {
            result.prioritySet = true;
        } else {
            addWarning(result,
                       std::string("failed to set SCHED_FIFO priority: ") + std::strerror(rc) +
                           " (typically requires CAP_SYS_NICE or root)");
        }
    }

    if (policy.cpuAffinity.has_value()) {
        const unsigned cpu = *policy.cpuAffinity;
        const unsigned available = std::thread::hardware_concurrency();
        if (available != 0 && cpu >= available) {
            addWarning(result, "requested CPU affinity " + std::to_string(cpu) +
                                    " is out of range (hardware_concurrency() = " +
                                    std::to_string(available) + ")");
        } else {
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            CPU_SET(cpu, &cpuset);
            const int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
            if (rc == 0) {
                result.affinitySet = true;
            } else {
                addWarning(result, std::string("failed to set CPU affinity: ") + std::strerror(rc));
            }
        }
    }

    if (policy.lockMemory) {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
            result.memoryLocked = true;
        } else {
            addWarning(result,
                       std::string("mlockall failed: ") + std::strerror(errno) +
                           " (check RLIMIT_MEMLOCK; raising it is a deployment-level setting, "
                           "e.g. systemd LimitMEMLOCK=infinity or /etc/security/limits.conf, "
                           "not something this process can grant itself)");
        }
    }

    return result;
}

#elif defined(_WIN32)

RtApplyResult applyRealtimePolicy(const RtPolicy& policy) {
    RtApplyResult result;

    if (policy.enableRealtimePriority) {
        if (SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
            result.prioritySet = true;
        } else {
            addWarning(result, "SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL) failed (error " +
                                    std::to_string(GetLastError()) + ")");
        }
    }

    if (policy.cpuAffinity.has_value()) {
        const unsigned cpu = *policy.cpuAffinity;
        const unsigned available = std::thread::hardware_concurrency();
        if (available != 0 && cpu >= available) {
            addWarning(result, "requested CPU affinity " + std::to_string(cpu) +
                                    " is out of range (hardware_concurrency() = " +
                                    std::to_string(available) + ")");
        } else {
            const DWORD_PTR mask = DWORD_PTR{1} << cpu;
            if (SetThreadAffinityMask(GetCurrentThread(), mask) != 0) {
                result.affinitySet = true;
            } else {
                addWarning(result, "SetThreadAffinityMask failed (error " +
                                        std::to_string(GetLastError()) + ")");
            }
        }
    }

    if (policy.lockMemory) {
        addWarning(result,
                   "memory locking is not supported on this platform (no process-wide mlockall "
                   "equivalent on Windows)");
    }

    return result;
}

#else

RtApplyResult applyRealtimePolicy(const RtPolicy& policy) {
    RtApplyResult result;
    if (policy.enableRealtimePriority) {
        addWarning(result, "real-time priority is not implemented on this platform");
    }
    if (policy.cpuAffinity.has_value()) {
        addWarning(result, "CPU affinity is not implemented on this platform");
    }
    if (policy.lockMemory) {
        addWarning(result, "memory locking is not implemented on this platform");
    }
    return result;
}

#endif

void prefaultStack(std::size_t bytes) {
    // Deliberately small and fixed-size (no alloca/VLA): pre-faulting is a
    // best-effort latency optimization, not a correctness requirement, and
    // allocating anything close to a full thread stack size here would risk a real
    // stack overflow. This follows the standard "stack_prefault" technique used by
    // real-time Linux tooling (e.g. cyclictest) of touching a modest, fixed chunk
    // of stack up front rather than the whole configured stack depth.
    constexpr std::size_t kBufferSize = 256 * 1024;
    unsigned char buffer[kBufferSize];
    const std::size_t toTouch = std::min(bytes, kBufferSize);
    std::memset(buffer, 0, toTouch);
    // Prevent the compiler from optimizing the write away entirely.
    volatile unsigned char sink = buffer[toTouch > 0 ? toTouch - 1 : 0];
    (void)sink;
}

}  // namespace softplc::core::rt
