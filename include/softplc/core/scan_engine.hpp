#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>

#include "softplc/core/program.hpp"
#include "softplc/io/io_driver.hpp"
#include "softplc/tags/tag_store.hpp"

namespace softplc::core {

struct ScanDiagnostics {
    std::uint64_t cycleCount = 0;
    std::uint64_t overrunCount = 0;
    std::chrono::microseconds lastCycleDuration{0};
    std::chrono::microseconds maxCycleDuration{0};
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
    std::jthread thread_;
};

}  // namespace softplc::core
