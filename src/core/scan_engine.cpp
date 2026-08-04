#include "softplc/core/scan_engine.hpp"

#include <algorithm>

namespace softplc::core {

ScanEngine::ScanEngine(tags::TagStore& tags, io::IIoDriver& io, std::shared_ptr<IProgram> program,
                        std::chrono::microseconds cycleTime)
    : tags_(tags), io_(io), program_(std::move(program)), cycleTime_(cycleTime) {
    cycleTimeTagId_ = tags_.find("System.CycleTime");
}

ScanEngine::~ScanEngine() { stop(); }

void ScanEngine::start() {
    if (thread_.joinable()) {
        return;
    }
    {
        std::lock_guard lock(rtReadyMutex_);
        rtReady_ = false;
    }
    thread_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });

    // Block until the scan thread has applied rtPolicy_ to itself, so
    // rtApplyResult() is race-free and accurate as soon as start() returns.
    std::unique_lock lock(rtReadyMutex_);
    rtReadyCv_.wait(lock, [this] { return rtReady_; });
}

void ScanEngine::stop() {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
}

bool ScanEngine::isRunning() const { return thread_.joinable(); }

void ScanEngine::setRtPolicy(rt::RtPolicy policy) {
    if (thread_.joinable()) {
        return;
    }
    rtPolicy_ = policy;
}

void ScanEngine::runOnce() {
    const auto cycleStart = std::chrono::steady_clock::now();

    ScanContext ctx{
        .cycleTime = cycleTime_,
        .cycleStart = cycleStart,
        .tags = tags_,
        .diagnostics = diagnostics_,
    };

    io_.readInputs(tags_);
    if (cycleTimeTagId_) {
        // Actual elapsed time since the previous scan started -- not the configured
        // (target) cycleTime_ -- so ST-defined timers (TON/TOF/...) stay accurate
        // under jitter/overrun instead of silently assuming the ideal cadence. The
        // very first scan has no previous sample, so it falls back to cycleTime_.
        const auto dt = lastScanStart_
                             ? std::chrono::duration_cast<tags::TimeValue>(cycleStart - *lastScanStart_)
                             : std::chrono::duration_cast<tags::TimeValue>(cycleTime_);
        tags_.write(*cycleTimeTagId_, dt);
    }
    lastScanStart_ = cycleStart;
    if (program_) {
        program_->execute(ctx);
    }
    io_.writeOutputs(tags_);

    const auto cycleEnd = std::chrono::steady_clock::now();
    const auto duration =
        std::chrono::duration_cast<std::chrono::microseconds>(cycleEnd - cycleStart);

    diagnostics_.cycleCount++;
    diagnostics_.lastCycleDuration = duration;
    diagnostics_.maxCycleDuration = std::max(diagnostics_.maxCycleDuration, duration);
    diagnostics_.minCycleDuration = std::min(diagnostics_.minCycleDuration, duration);
    if (duration > cycleTime_) {
        diagnostics_.overrunCount++;
    }
}

void ScanEngine::run(std::stop_token stopToken) {
    if (rtPolicy_.lockMemory) {
        rt::prefaultStack();
    }
    rtApplyResult_ = rt::applyRealtimePolicy(rtPolicy_);
    {
        std::lock_guard lock(rtReadyMutex_);
        rtReady_ = true;
    }
    rtReadyCv_.notify_all();

    // Hold a fixed schedule (nextTick accumulates by cycleTime_ each iteration)
    // rather than resetting the baseline to now() every loop: resetting would
    // silently absorb any OS scheduling delay into cadence drift instead of
    // surfacing it as jitter.
    auto nextTick = std::chrono::steady_clock::now();
    while (!stopToken.stop_requested()) {
        const auto wake = std::chrono::steady_clock::now();
        // First iteration's jitter reading is near-zero/meaningless (nextTick was
        // just set to the current time above) — expected, not a bug.
        const auto jitter = std::chrono::duration_cast<std::chrono::microseconds>(wake - nextTick);
        diagnostics_.lastWakeJitter = jitter;
        diagnostics_.maxWakeJitter = std::max(diagnostics_.maxWakeJitter, jitter);

        runOnce();

        nextTick += cycleTime_;
        if (nextTick <= std::chrono::steady_clock::now()) {
            // Fell a full cycle or more behind: resync to "now + one cycle" rather
            // than firing several back-to-back catch-up scans, which would just
            // compound overruns under sustained overload.
            diagnostics_.resyncCount++;
            nextTick = std::chrono::steady_clock::now() + cycleTime_;
        } else {
            std::this_thread::sleep_until(nextTick);
        }
    }
}

}  // namespace softplc::core
