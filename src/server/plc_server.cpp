#include "softplc/server/plc_server.hpp"

#include <httplib.h>

#include <chrono>
#include <sstream>

#include "softplc/tags/value.hpp"

namespace softplc::server {

namespace {

std::string addressToString(const tags::Address& addr) {
    char areaChar = '?';
    switch (addr.area) {
        case tags::MemoryArea::Input:
            areaChar = 'I';
            break;
        case tags::MemoryArea::Output:
            areaChar = 'Q';
            break;
        case tags::MemoryArea::Memory:
            areaChar = 'M';
            break;
        case tags::MemoryArea::None:
            break;
    }
    std::ostringstream out;
    out << '%' << areaChar << addr.byteOffset;
    if (addr.bitOffset != tags::Address::kNoBit) {
        out << '.' << static_cast<int>(addr.bitOffset);
    }
    return out.str();
}

std::string tagToJson(const tags::Tag& tag) {
    std::ostringstream out;
    out << "{\"name\":" << tags::jsonEscapeString(tag.name) << ",\"type\":\""
        << tags::toString(tag.type) << "\",\"value\":" << tags::toJson(tag.value);
    if (tag.address) {
        out << ",\"address\":\"" << addressToString(*tag.address) << "\"";
    }
    out << "}";
    return out.str();
}

std::string tagsToJson(const std::vector<tags::Tag>& snapshot) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        if (i != 0) {
            out << ",";
        }
        out << tagToJson(snapshot[i]);
    }
    out << "]";
    return out.str();
}

std::string errorJson(const std::string& message) {
    return "{\"error\":" + tags::jsonEscapeString(message) + "}";
}

}  // namespace

PlcServer::PlcServer(io::IIoDriver& io, std::chrono::microseconds cycleTime)
    : io_(io), cycleTime_(cycleTime), tags_(std::make_unique<tags::TagStore>()),
      http_(std::make_unique<httplib::Server>()) {
    registerRoutes();
}

PlcServer::~PlcServer() { stop(); }

DownloadResult PlcServer::download(const std::string& stSource) {
    auto newTags = std::make_unique<tags::TagStore>();
    std::shared_ptr<st::StProgram> newProgram;
    try {
        newProgram = st::StProgram::load(stSource, *newTags);
    } catch (const std::exception& e) {
        return DownloadResult{.ok = false, .error = e.what(), .programName = {}, .tagCount = 0};
    }

    std::unique_lock lock(stateMutex_);
    // Destroying the old engine (if any) joins its scan thread before the new engine
    // is constructed, so the two never call into io_ concurrently.
    engine_.reset();
    tags_ = std::move(newTags);
    program_ = std::move(newProgram);
    engine_ = std::make_unique<core::ScanEngine>(*tags_, io_, program_, cycleTime_);
    engine_->start();
    programName_ = std::string(program_->name());

    return DownloadResult{
        .ok = true, .error = {}, .programName = programName_, .tagCount = tags_->size()};
}

std::vector<tags::Tag> PlcServer::tagSnapshot() const {
    std::shared_lock lock(stateMutex_);
    return tags_->snapshot();
}

bool PlcServer::isRunning() const {
    std::shared_lock lock(stateMutex_);
    return engine_ && engine_->isRunning();
}

std::optional<core::ScanDiagnostics> PlcServer::diagnostics() const {
    std::shared_lock lock(stateMutex_);
    if (!engine_) {
        return std::nullopt;
    }
    return engine_->diagnostics();
}

std::string PlcServer::programName() const {
    std::shared_lock lock(stateMutex_);
    return programName_;
}

void PlcServer::registerRoutes() {
    http_->Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("SOFT-PLC programming/monitoring server. See /api/status.\n",
                         "text/plain");
    });

    http_->Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        std::ostringstream out;
        out << "{\"running\":" << (isRunning() ? "true" : "false") << ",\"programName\":"
            << tags::jsonEscapeString(programName()) << ",\"tagCount\":"
            << tagSnapshot().size();
        if (const auto diag = diagnostics()) {
            out << ",\"diagnostics\":{\"cycleCount\":" << diag->cycleCount
                << ",\"overrunCount\":" << diag->overrunCount
                << ",\"lastCycleDurationUs\":" << diag->lastCycleDuration.count() << "}";
        }
        out << "}";
        res.set_content(out.str(), "application/json");
    });

    http_->Get("/api/tags", [this](const httplib::Request&, httplib::Response& res) {
        res.set_content(tagsToJson(tagSnapshot()), "application/json");
    });

    // Server-sent-events tag stream: a full snapshot every 200ms until the client
    // disconnects. v1 deliberately polls on a fixed wall-clock interval rather than
    // being scan-synchronized (good enough for a human-facing monitor UI, not a
    // control-loop feed) and holds one httplib worker thread per connected client for
    // the lifetime of the connection -- documented as a known limitation, not a
    // fix-now issue, since a handful of GUI viewers is the only expected load.
    http_->Get("/api/tags/stream", [this](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
                if (!sink.is_writable()) {
                    return false;
                }
                const std::string chunk = "data: " + tagsToJson(tagSnapshot()) + "\n\n";
                if (!sink.write(chunk.data(), chunk.size())) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                return true;
            });
    });

    http_->Post("/api/program", [this](const httplib::Request& req, httplib::Response& res) {
        const DownloadResult result = download(req.body);
        if (!result.ok) {
            res.status = 400;
            res.set_content(errorJson(result.error), "application/json");
            return;
        }
        std::ostringstream out;
        out << "{\"programName\":" << tags::jsonEscapeString(result.programName)
            << ",\"tagCount\":" << result.tagCount << "}";
        res.set_content(out.str(), "application/json");
    });
}

bool PlcServer::listen(const std::string& host, int port) { return http_->listen(host, port); }

int PlcServer::bindEphemeralPort(const std::string& host) {
    return http_->bind_to_any_port(host);
}

bool PlcServer::listenAfterBind() { return http_->listen_after_bind(); }

void PlcServer::stop() {
    http_->stop();
    std::unique_lock lock(stateMutex_);
    engine_.reset();
}

}  // namespace softplc::server
