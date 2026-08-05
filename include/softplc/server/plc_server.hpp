#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
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

struct ProjectSummary {
    std::string name;
    std::string updatedAt;  // ISO-8601 UTC, from the project file's filesystem mtime
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
    // `staticDir`, if non-empty, is mounted at "/" (cpp-httplib's set_mount_point) to
    // serve a built web frontend -- e.g. web/dist's `index.html`/JS/CSS -- so a browser
    // pointed at this server gets the actual GUI, not just the JSON API. Left empty,
    // "/" instead serves a one-line plain-text banner (useful for apps/plc_server
    // usage without a frontend built yet, or the test suite, which has no dist/ to
    // point at). Decided at construction time, not changeable afterwards -- there's no
    // use case yet for switching a running server's static directory.
    // `projectsDir`, if non-empty, enables the project-storage endpoints (see "Project
    // storage" in docs/architecture.md): each project is one opaque JSON file, saved
    // and read back verbatim -- the schema (blocks/networks/DBs/IO linking/drive
    // config) is entirely the frontend's concern, compiled client-side into ST source
    // (see web/src/project/compile.ts). The one field PlcServer itself ever looks
    // inside a project for is "compiledSource", read back by autoLoadActiveProject()
    // below.
    PlcServer(io::IIoDriver& io, std::chrono::microseconds cycleTime,
              std::string staticDir = {}, std::string projectsDir = {});
    ~PlcServer();

    PlcServer(const PlcServer&) = delete;
    PlcServer& operator=(const PlcServer&) = delete;

    // Compiles `stSource` and, on success, stops any currently-running engine (fully,
    // before starting the new one -- never two engines driving the same IIoDriver at
    // once) and starts a new one against a fresh TagStore. On failure (a
    // LexError/ParseError/std::runtime_error from st::StProgram::load()) the
    // currently-running engine is left running, untouched.
    DownloadResult download(const std::string& stSource);

    struct WriteResult {
        bool ok = false;
        std::string error;  // populated iff !ok
    };

    // Writes `rawValue` into the live tag `name`, coercing it to the tag's declared
    // type the same way ST's own AssignStmt narrowing does (tags::coerceToType) -- so
    // e.g. a plain `double` (what a JSON number decodes to, see server/plc_server.cpp)
    // narrows correctly into an INT/DINT/REAL/BOOL/STRING-declared tag. Fails for an
    // unknown tag name, no program loaded, or a genuine type mismatch (e.g. a string
    // into a DINT tag).
    WriteResult writeTag(const std::string& name, const tags::Value& rawValue);

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

    // Project storage (no-ops / empty results if constructed with no projectsDir).
    // `name` is validated against a strict allowlist (see isValidProjectName() in
    // plc_server.cpp) before it ever reaches a filesystem path -- project names come
    // from an HTTP path segment, so this is the load-bearing defense against path
    // traversal (a name like "../../etc/passwd").
    [[nodiscard]] std::vector<ProjectSummary> listProjects() const;
    // The raw JSON text previously saved for `name`, or std::nullopt if it doesn't
    // exist (or `name` is invalid).
    [[nodiscard]] std::optional<std::string> getProject(const std::string& name) const;
    // Writes `json` verbatim as that project's saved file. Returns false for an
    // invalid name; does not otherwise inspect or validate `json`'s contents.
    bool saveProject(const std::string& name, const std::string& json);
    bool deleteProject(const std::string& name);
    // The currently "active" project name (the one autoLoadActiveProject() below acts
    // on at startup), or std::nullopt if none has been set or no project storage is
    // configured.
    [[nodiscard]] std::optional<std::string> activeProjectName() const;
    // Marks `name` active; fails (false) if that project doesn't exist.
    bool setActiveProject(const std::string& name);

    struct AutoLoadResult {
        bool attempted = false;  // true iff there was an active project with a
                                  // non-empty compiledSource to try downloading
        DownloadResult download;
    };
    // Reads the active project's saved "compiledSource" field (if any) and downloads
    // it -- the "already-loaded program automatically starts" behavior. Intended to
    // be called once, by apps/plc_server/main.cpp, before entering the listen loop;
    // not called automatically by the constructor so construction stays side-effect-
    // free (matching how an initial program path is handled explicitly by main.cpp,
    // not by PlcServer itself).
    AutoLoadResult autoLoadActiveProject();

private:
    void registerRoutes();
    [[nodiscard]] std::filesystem::path projectFilePath(const std::string& name) const;

    io::IIoDriver& io_;
    std::chrono::microseconds cycleTime_;
    std::string staticDir_;
    std::string projectsDir_;

    mutable std::shared_mutex stateMutex_;  // guards every field below
    std::unique_ptr<tags::TagStore> tags_;
    std::shared_ptr<st::StProgram> program_;
    std::unique_ptr<core::ScanEngine> engine_;
    std::string programName_;

    std::unique_ptr<httplib::Server> http_;
};

}  // namespace softplc::server
