#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "softplc/core/program.hpp"
#include "softplc/core/rt_scheduling.hpp"
#include "softplc/io/io_driver.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::core {

struct ScanDiagnostics {
    std::uint64_t cycleCount = 0;
    std::uint64_t overrunCount = 0;
    std::chrono::microseconds lastCycleDuration{0};
    std::chrono::microseconds maxCycleDuration{0};
    std::chrono::microseconds minCycleDuration{std::chrono::microseconds::max()};

    // Populated only by the threaded run() loop (not by standalone runOnce() calls):
    // how late the thread woke relative to its fixed schedule, and how many times
    // the schedule had to resync after falling a full cycle behind.
    std::chrono::microseconds lastWakeJitter{0};
    std::chrono::microseconds maxWakeJitter{0};
    std::uint64_t resyncCount = 0;
};

struct ScanContext {
    std::chrono::microseconds cycleTime;
    std::chrono::steady_clock::time_point cycleStart;
    tags::TagStore& tags;
    ScanDiagnostics& diagnostics;
};

// Owns the PLC scan cycle: read inputs -> execute program -> write outputs ->
// housekeeping, run at a fixed configurable cycle time on a background thread.
//
// runOnce() performs exactly one scan synchronously and is exposed independently of
// the threaded loop so tests can drive deterministic scans without relying on real
// timing. An overrun (a scan taking longer than cycleTime) is recorded in
// diagnostics() rather than treated as fatal.
class ScanEngine {
public:
    ScanEngine(tags::TagStore& tags, io::IIoDriver& io, std::shared_ptr<IProgram> program,
               std::chrono::microseconds cycleTime);
    ~ScanEngine();

    ScanEngine(const ScanEngine&) = delete;
    ScanEngine& operator=(const ScanEngine&) = delete;

    // Sets the OS-level real-time scheduling policy to apply (priority/affinity/
    // memory locking) to the scan thread. Must be called before start(); no-op if
    // the engine is already running (mirrors start()/stop()'s no-op convention).
    void setRtPolicy(rt::RtPolicy policy);
    // Result of applying the RT policy to the scan thread. start() blocks until the
    // scan thread has actually applied the policy, so this is always up to date by
    // the time start() returns (no need to poll or synchronize further).
    [[nodiscard]] const rt::RtApplyResult& rtApplyResult() const { return rtApplyResult_; }

    // Starts the timed scan loop on a background thread. No-op if already running.
    void start();
    // Requests the background loop to stop and joins it. No-op if not running.
    void stop();
    [[nodiscard]] bool isRunning() const;

    // Runs exactly one scan cycle synchronously.
    void runOnce();

    [[nodiscard]] const ScanDiagnostics& diagnostics() const { return diagnostics_; }

private:
    void run(std::stop_token stopToken);

    tags::TagStore& tags_;
    io::IIoDriver& io_;
    std::shared_ptr<IProgram> program_;
    std::chrono::microseconds cycleTime_;
    ScanDiagnostics diagnostics_;
    rt::RtPolicy rtPolicy_;
    rt::RtApplyResult rtApplyResult_;
    // Guards the handoff of rtApplyResult_ from the scan thread (which applies the
    // policy to itself at the top of run()) to start(), which blocks until that's
    // done so the result is race-free and immediately readable once start() returns.
    std::mutex rtReadyMutex_;
    std::condition_variable rtReadyCv_;
    bool rtReady_ = false;
    std::jthread thread_;

    // Resolved once (if present) at construction time: the well-known "System.
    // CycleTime" TIME tag that ST-defined timer FBs (TON/TOF/...) accumulate
    // elapsed time against. Absent (nullopt) for a TagStore that never went
    // through st::StProgram::load() -- no cost, no behavior change for those.
    std::optional<tags::TagId> cycleTimeTagId_;
    // Wall-clock start of the previous scan, used to compute the actual elapsed
    // time published to cycleTimeTagId_ each scan (not the configured cycleTime_,
    // which is only a target). Unset before the first scan.
    std::optional<std::chrono::steady_clock::time_point> lastScanStart_;
};

}  // namespace softplc::core
