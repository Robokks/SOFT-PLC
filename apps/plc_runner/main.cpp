#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "softplc/core/rt_scheduling.hpp"
#include "softplc/core/scan_engine.hpp"
#include "softplc/io/simulated_io_driver.hpp"
#include "softplc/st/st_program.hpp"
#include "softplc/tags/tag_store.hpp"

namespace {

std::atomic<bool> g_stopRequested{false};

void handleSigint(int /*signal*/) { g_stopRequested = true; }

std::string readFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("could not open file: " + path);
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string valueToString(const softplc::tags::Value& value) {
    return std::visit(
        [](auto&& v) -> std::string {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                return v ? "TRUE" : "FALSE";
            } else if constexpr (std::is_same_v<T, softplc::tags::TimeValue>) {
                return std::to_string(v.count()) + "ms";
            } else if constexpr (std::is_same_v<T, std::string>) {
                return "'" + v + "'";
            } else {
                return std::to_string(v);
            }
        },
        value);
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    softplc::core::rt::RtPolicy rtPolicy;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (startsWith(arg, "--rt-priority=")) {
            rtPolicy.enableRealtimePriority = true;
            rtPolicy.priority = std::stoi(arg.substr(std::string("--rt-priority=").size()));
        } else if (startsWith(arg, "--rt-affinity=")) {
            rtPolicy.cpuAffinity =
                static_cast<unsigned>(std::stoi(arg.substr(std::string("--rt-affinity=").size())));
        } else if (arg == "--lock-memory") {
            rtPolicy.lockMemory = true;
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.empty()) {
        std::cerr << "usage: " << argv[0]
                  << " <program.st> [cycle_time_ms] [--rt-priority=N] [--rt-affinity=N] "
                     "[--lock-memory]\n";
        return 1;
    }

    const std::string path = positional[0];
    const int cycleMs = positional.size() >= 2 ? std::stoi(positional[1]) : 10;

    softplc::tags::TagStore tags;
    std::shared_ptr<softplc::st::StProgram> program;
    try {
        const std::string source = readFile(path);
        program = softplc::st::StProgram::load(source, tags);
    } catch (const std::exception& e) {
        std::cerr << "failed to load '" << path << "': " << e.what() << '\n';
        return 1;
    }

    std::cout << "loaded program '" << program->name() << "' (" << tags.size()
              << " tags), cycle time " << cycleMs << "ms\n";

    softplc::io::SimulatedIoDriver io;
    softplc::core::ScanEngine engine(tags, io, program, std::chrono::milliseconds(cycleMs));
    engine.setRtPolicy(rtPolicy);

    std::signal(SIGINT, handleSigint);
    engine.start();

    for (const auto& warning : engine.rtApplyResult().warnings) {
        std::cerr << "rt warning: " << warning << '\n';
    }

    std::cout << "running (Ctrl+C to stop)...\n";
    while (!g_stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        tags.forEachInArea(softplc::tags::MemoryArea::Output, [](const softplc::tags::Tag& tag) {
            std::cout << "  " << tag.name << " = " << valueToString(tag.value) << '\n';
        });
    }

    engine.stop();

    const auto& diag = engine.diagnostics();
    std::cout << "stopped. cycles=" << diag.cycleCount << " overruns=" << diag.overrunCount
              << " maxCycleDuration=" << diag.maxCycleDuration.count() << "us"
              << " maxWakeJitter=" << diag.maxWakeJitter.count() << "us"
              << " resyncs=" << diag.resyncCount << '\n';
    return 0;
}
