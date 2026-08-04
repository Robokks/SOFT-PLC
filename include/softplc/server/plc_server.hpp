#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "softplc/core/scan_engine.hpp"
#include "softplc/io/io_driver.hpp"
#include "softplc/st/st_program.hpp"
#include "softplc/tags/tag_store.hpp"

// Forward-declared rather than included: httplib is a large single-header library and
// this is the one public header of this module, so keep it out of every translation
// unit that merely wants to construct/drive a PlcServer.
namespace httplib {
class Server;
}

namespace softplc::server {

struct DownloadResult {
    bool ok = false;
    std::string error;  // populated iff !ok; the currently-running program (if any) is
                         // left completely untouched when this is set.
    std::string programName;
    std::size_t tagCount = 0;
};

// Hosts an HTTP API in front of a live PLC runtime, for a future browser-based GUI:
// compiling/"downloading" a new ST source into a running target, and exposing the live
// TagStore for an online tag monitor. See docs/architecture.md's "Programming/
// monitoring HTTP server" section for the design rationale (why a whole-object-graph
// swap on download, why SSE instead of WebSocket, why no TLS in v1).
//
// Owns the currently-loaded {TagStore, StProgram, ScanEngine} as a single unit, guarded
// by stateMutex_: HTTP handlers (running on cpp-httplib's own worker threads) take the
// shared lock to read tags/diagnostics, download() takes the exclusive lock to replace
// the whole unit. The IIoDriver is long-lived, owned by the caller, and reused across
// downloads -- readInputs()/writeOutputs() take the TagStore as a parameter rather than
// caching it, so a fresh TagStore each download is transparent to it (see
// io/io_driver.hpp).
class PlcServer {
public:
    PlcServer(io::IIoDriver& io, std::chrono::microseconds cycleTime);
    ~PlcServer();

    PlcServer(const PlcServer&) = delete;
    PlcServer& operator=(const PlcServer&) = delete;

    // Compiles `stSource` and, on success, stops any currently-running engine (fully,
    // before starting the new one -- never two engines driving the same IIoDriver at
    // once) and starts a new one against a fresh TagStore. On failure (a
    // LexError/ParseError/std::runtime_error from st::StProgram::load()) the
    // currently-running engine is left running, untouched.
    DownloadResult download(const std::string& stSource);

    // Starts the HTTP listener. Blocks until the server stops (call from a dedicated
    // thread if the caller needs to keep running). Returns false if the port couldn't
    // be bound.
    bool listen(const std::string& host, int port);

    // Binds to an OS-assigned ephemeral port without blocking, returning it (0 on
    // failure) -- for callers (tests) that need to know which port to connect a client
    // to before the server thread has started accepting. Follow with a call to
    // listenAfterBind() (typically on a background thread).
    int bindEphemeralPort(const std::string& host);
    bool listenAfterBind();
    // Requests the blocking listen() call above to return, and joins any running scan
    // engine. Safe to call from another thread.
    void stop();

    [[nodiscard]] std::vector<tags::Tag> tagSnapshot() const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] std::optional<core::ScanDiagnostics> diagnostics() const;
    [[nodiscard]] std::string programName() const;

private:
    void registerRoutes();

    io::IIoDriver& io_;
    std::chrono::microseconds cycleTime_;

    mutable std::shared_mutex stateMutex_;  // guards every field below
    std::unique_ptr<tags::TagStore> tags_;
    std::shared_ptr<st::StProgram> program_;
    std::unique_ptr<core::ScanEngine> engine_;
    std::string programName_;

    std::unique_ptr<httplib::Server> http_;
};

}  // namespace softplc::server
