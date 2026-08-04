#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "softplc/io/simulated_io_driver.hpp"
#include "softplc/server/plc_server.hpp"

namespace {

std::atomic<bool> g_stopRequested{false};

// Only touches a std::atomic -- deliberately not calling into PlcServer::stop() (which
// takes a mutex and closes sockets) directly from signal-handler context, same
// atomic-flag-polled-from-main-thread convention apps/plc_runner/main.cpp uses.
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

}  // namespace

// Thin executable: constructs a PlcServer over a SimulatedIoDriver and listens for the
// programming/monitoring HTTP API (see docs/architecture.md). Unlike plc_runner (which
// always runs one fixed program loaded from argv), an initial program here is optional
// -- a program can instead be "downloaded" later via POST /api/program, matching how a
// real PLC target can sit idle with nothing loaded until a programming tool connects.
int main(int argc, char** argv) {
    std::string host = "0.0.0.0";
    int port = 8080;
    int cycleMs = 10;
    std::string staticDir;
    std::string initialProgramPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--host=", 0) == 0) {
            host = arg.substr(7);
        } else if (arg.rfind("--port=", 0) == 0) {
            port = std::stoi(arg.substr(7));
        } else if (arg.rfind("--cycle-time-ms=", 0) == 0) {
            cycleMs = std::stoi(arg.substr(16));
        } else if (arg.rfind("--static-dir=", 0) == 0) {
            staticDir = arg.substr(13);
        } else if (initialProgramPath.empty()) {
            initialProgramPath = arg;
        }
    }

    softplc::io::SimulatedIoDriver io;
    softplc::server::PlcServer server(io, std::chrono::milliseconds(cycleMs), staticDir);

    if (!initialProgramPath.empty()) {
        const auto result = server.download(readFile(initialProgramPath));
        if (!result.ok) {
            std::cerr << "failed to load '" << initialProgramPath << "': " << result.error
                      << '\n';
            return 1;
        }
        std::cout << "loaded program '" << result.programName << "' (" << result.tagCount
                  << " tags)\n";
    }

    std::signal(SIGINT, handleSigint);

    if (!staticDir.empty()) {
        std::cout << "serving web frontend from '" << staticDir << "'\n";
    }
    std::cout << "listening on http://" << host << ":" << port << " (Ctrl+C to stop)...\n";
    bool bindFailed = false;
    std::thread listener([&] {
        if (!server.listen(host, port)) {
            bindFailed = true;
            g_stopRequested = true;
        }
    });

    while (!g_stopRequested) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    server.stop();
    listener.join();

    if (bindFailed) {
        std::cerr << "failed to bind " << host << ":" << port << '\n';
        return 1;
    }
    return 0;
}
