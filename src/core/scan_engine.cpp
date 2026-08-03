#include "softplc/core/scan_engine.hpp"

#include <algorithm>

namespace softplc::core {

ScanEngine::ScanEngine(tags::TagStore& tags, io::IIoDriver& io, std::shared_ptr<IProgram> program,
                        std::chrono::microseconds cycleTime)
    : tags_(tags), io_(io), program_(std::move(program)), cycleTime_(cycleTime) {}

ScanEngine::~ScanEngine() { stop(); }

void ScanEngine::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
}

void ScanEngine::stop() {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
}

bool ScanEngine::isRunning() const { return thread_.joinable(); }

void ScanEngine::runOnce() {
    const auto cycleStart = std::chrono::steady_clock::now();

    ScanContext ctx{
        .cycleTime = cycleTime_,
        .cycleStart = cycleStart,
        .tags = tags_,
        .diagnostics = diagnostics_,
    };

    io_.readInputs(tags_);
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
    if (duration > cycleTime_) {
        diagnostics_.overrunCount++;
    }
}

void ScanEngine::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        const auto tickStart = std::chrono::steady_clock::now();
        runOnce();
        std::this_thread::sleep_until(tickStart + cycleTime_);
    }
}

}  // namespace softplc::core
