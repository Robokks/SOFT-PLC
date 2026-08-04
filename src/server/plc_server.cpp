#include "softplc/server/plc_server.hpp"

#include <httplib.h>

#include <cctype>
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

struct ParsedJsonValue {
    enum class Kind { Invalid, Bool, Number, String } kind = Kind::Invalid;
    bool boolValue = false;
    double numberValue = 0;
    std::string stringValue;
};

// Extracts and decodes the single "value" field of a `{"value": <literal>}` request
// body -- deliberately not a general JSON parser (see "JSON is hand-written" in
// docs/architecture.md): the only structured JSON this server ever needs to *read* is
// this one shape, for the tag-write endpoint below.
ParsedJsonValue parseValueField(const std::string& body) {
    ParsedJsonValue result;
    const auto keyPos = body.find("\"value\"");
    if (keyPos == std::string::npos) {
        return result;
    }
    auto pos = body.find(':', keyPos + 7);
    if (pos == std::string::npos) {
        return result;
    }
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos]))) {
        ++pos;
    }
    if (pos >= body.size()) {
        return result;
    }

    if (body[pos] == '"') {
        std::string decoded;
        ++pos;
        while (pos < body.size() && body[pos] != '"') {
            char c = body[pos];
            if (c == '\\' && pos + 1 < body.size()) {
                ++pos;
                switch (body[pos]) {
                    case 'n':
                        decoded += '\n';
                        break;
                    case 't':
                        decoded += '\t';
                        break;
                    case 'r':
                        decoded += '\r';
                        break;
                    default:
                        decoded += body[pos];  // handles \" \\ \/ verbatim
                        break;
                }
            } else {
                decoded += c;
            }
            ++pos;
        }
        if (pos >= body.size()) {
            return result;  // unterminated string
        }
        result.kind = ParsedJsonValue::Kind::String;
        result.stringValue = std::move(decoded);
        return result;
    }

    if (body.compare(pos, 4, "true") == 0) {
        result.kind = ParsedJsonValue::Kind::Bool;
        result.boolValue = true;
        return result;
    }
    if (body.compare(pos, 5, "false") == 0) {
        result.kind = ParsedJsonValue::Kind::Bool;
        result.boolValue = false;
        return result;
    }

    std::size_t end = pos;
    while (end < body.size() && (std::isdigit(static_cast<unsigned char>(body[end])) != 0 ||
                                  body[end] == '-' || body[end] == '+' || body[end] == '.' ||
                                  body[end] == 'e' || body[end] == 'E')) {
        ++end;
    }
    if (end == pos) {
        return result;
    }
    try {
        result.numberValue = std::stod(body.substr(pos, end - pos));
        result.kind = ParsedJsonValue::Kind::Number;
    } catch (const std::exception&) {
        // leave Kind::Invalid
    }
    return result;
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

PlcServer::WriteResult PlcServer::writeTag(const std::string& name, const tags::Value& rawValue) {
    std::shared_lock lock(stateMutex_);
    const auto id = tags_->find(name);
    if (!id) {
        return WriteResult{.ok = false, .error = "unknown tag: " + name};
    }
    const tags::TypeId target = tags_->typeOf(*id);
    try {
        // JSON numbers arrive as a plain double (see parseValueField); TIME has no
        // numeric Value representation for coerceToType() to narrow into, so it's
        // special-cased here rather than taught to coerceToType, which every other
        // caller (the ST interpreter) never needs for a bare double.
        if (target == tags::TypeId::Time && std::holds_alternative<double>(rawValue)) {
            tags_->write(*id, tags::TimeValue(static_cast<std::int64_t>(std::get<double>(rawValue))));
        } else {
            tags_->write(*id, tags::coerceToType(rawValue, target));
        }
        return WriteResult{.ok = true, .error = {}};
    } catch (const std::exception& e) {
        return WriteResult{.ok = false, .error = e.what()};
    }
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

    // Tag write/"force": POST /api/tags/<name> with body {"value": <bool|number|string>}.
    http_->Post(R"(/api/tags/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string tagName = req.matches[1];
        const ParsedJsonValue parsed = parseValueField(req.body);

        tags::Value rawValue;
        switch (parsed.kind) {
            case ParsedJsonValue::Kind::Bool:
                rawValue = parsed.boolValue;
                break;
            case ParsedJsonValue::Kind::Number:
                rawValue = parsed.numberValue;
                break;
            case ParsedJsonValue::Kind::String:
                rawValue = parsed.stringValue;
                break;
            case ParsedJsonValue::Kind::Invalid:
                res.status = 400;
                res.set_content(errorJson("expected a JSON body of the form "
                                           "{\"value\": <bool|number|string>}"),
                                 "application/json");
                return;
        }

        const WriteResult result = writeTag(tagName, rawValue);
        if (!result.ok) {
            res.status = 400;
            res.set_content(errorJson(result.error), "application/json");
            return;
        }
        res.set_content("{\"ok\":true}", "application/json");
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
