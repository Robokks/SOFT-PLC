#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <type_traits>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <program.st> [cycle_time_ms]\n";
        return 1;
    }

    const std::string path = argv[1];
    const int cycleMs = argc >= 3 ? std::stoi(argv[2]) : 10;

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

    std::signal(SIGINT, handleSigint);
    engine.start();

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
              << " maxCycleDuration=" << diag.maxCycleDuration.count() << "us\n";
    return 0;
}
