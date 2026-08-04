#include "softplc/server/plc_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "softplc/io/simulated_io_driver.hpp"

using namespace softplc;

namespace {

// Wraps a PlcServer bound to an ephemeral port, listening on a background thread for
// the lifetime of the fixture -- mirrors tests/support/mock_modbus_server's
// real-server-not-a-mock approach: every test below drives the actual HTTP server
// through a real httplib::Client, not a hand-mocked transport.
class PlcServerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<server::PlcServer>(io_, std::chrono::milliseconds(5));
        port_ = server_->bindEphemeralPort("127.0.0.1");
        ASSERT_GT(port_, 0);
        listener_ = std::thread([this] { server_->listenAfterBind(); });
        client_ = std::make_unique<httplib::Client>("127.0.0.1", port_);
        client_->set_connection_timeout(2);
        client_->set_read_timeout(2);
    }

    void TearDown() override {
        server_->stop();
        listener_.join();
    }

    io::SimulatedIoDriver io_;
    std::unique_ptr<server::PlcServer> server_;
    std::unique_ptr<httplib::Client> client_;
    std::thread listener_;
    int port_ = 0;
};

constexpr auto kCounterProgram = R"(
    PROGRAM Test
    VAR
        Counter : DINT := 0;
    END_VAR
    Counter := Counter + 1;
    END_PROGRAM
)";

// A program that only ever reads its VAR_INPUT-like tags (never writes them itself),
// so a written value is observable unchanged on the next /api/tags read -- proof that
// the write endpoint, not scan logic, produced it.
constexpr auto kForceableTagsProgram = R"(
    PROGRAM Test
    VAR
        Speed : INT := 0;
        Enabled : BOOL := FALSE;
        Setpoint : REAL := 0.0;
        Label : STRING := 'none';
    END_VAR
    Label := Label;
    END_PROGRAM
)";

}  // namespace

TEST_F(PlcServerFixture, StartsWithNoProgramLoaded) {
    EXPECT_FALSE(server_->isRunning());
    EXPECT_EQ(server_->programName(), "");
    EXPECT_TRUE(server_->tagSnapshot().empty());

    const auto res = client_->Get("/api/status");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("\"running\":false"), std::string::npos);
}

TEST_F(PlcServerFixture, PostProgramCompilesAndStartsIt) {
    const auto res = client_->Post("/api/program", kCounterProgram, "text/plain");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("\"programName\":\"Test\""), std::string::npos);

    EXPECT_TRUE(server_->isRunning());
    EXPECT_EQ(server_->programName(), "Test");
}

TEST_F(PlcServerFixture, InvalidProgramLeavesPriorEngineUntouched) {
    ASSERT_TRUE(client_->Post("/api/program", kCounterProgram, "text/plain"));
    ASSERT_TRUE(server_->isRunning());

    const auto res = client_->Post("/api/program", "not valid ST at all {{{", "text/plain");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
    EXPECT_NE(res->body.find("\"error\""), std::string::npos);

    // The previously-downloaded, valid program must still be the one running.
    EXPECT_TRUE(server_->isRunning());
    EXPECT_EQ(server_->programName(), "Test");
}

TEST_F(PlcServerFixture, TagsEndpointReflectsLiveScanUpdates) {
    ASSERT_TRUE(client_->Post("/api/program", kCounterProgram, "text/plain"));

    // Let a handful of 5ms scans run so Counter has visibly advanced past 0.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto res = client_->Get("/api/tags");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    EXPECT_NE(res->body.find("\"name\":\"Counter\""), std::string::npos);
    EXPECT_NE(res->body.find("\"type\":\"DINT\""), std::string::npos);
    // Counter must have counted up from its initializer, not sat at 0.
    EXPECT_EQ(res->body.find("\"value\":0"), std::string::npos);
}

TEST_F(PlcServerFixture, WriteEndpointForcesBoolIntRealAndStringTags) {
    ASSERT_TRUE(client_->Post("/api/program", kForceableTagsProgram, "text/plain"));

    auto post = [this](const std::string& tag, const std::string& jsonBody) {
        return client_->Post("/api/tags/" + tag, jsonBody, "application/json");
    };

    {
        const auto res = post("Enabled", R"({"value": true})");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }
    {
        const auto res = post("Speed", R"({"value": 1500})");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }
    {
        const auto res = post("Setpoint", R"({"value": 3.5})");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }
    {
        const auto res = post("Label", R"({"value": "hello"})");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 200);
    }

    const auto tagsRes = client_->Get("/api/tags");
    ASSERT_TRUE(tagsRes);
    EXPECT_NE(tagsRes->body.find("\"name\":\"Enabled\",\"type\":\"BOOL\",\"value\":true"),
              std::string::npos);
    EXPECT_NE(tagsRes->body.find("\"name\":\"Speed\",\"type\":\"INT\",\"value\":1500"),
              std::string::npos);
    // REAL/LREAL serialize via std::to_string's fixed 6-decimal form (see
    // docs/architecture.md's "JSON is hand-written" note), not a minimal
    // representation -- 3.5 becomes "3.500000", not "3.5".
    EXPECT_NE(
        tagsRes->body.find("\"name\":\"Setpoint\",\"type\":\"REAL\",\"value\":3.500000"),
        std::string::npos);
    EXPECT_NE(tagsRes->body.find(R"("name":"Label","type":"STRING","value":"hello")"),
              std::string::npos);
}

TEST_F(PlcServerFixture, WriteEndpointRejectsUnknownTagAndTypeMismatch) {
    ASSERT_TRUE(client_->Post("/api/program", kForceableTagsProgram, "text/plain"));

    {
        const auto res = client_->Post("/api/tags/NoSuchTag", R"({"value": 1})",
                                        "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400);
        EXPECT_NE(res->body.find("\"error\""), std::string::npos);
    }
    {
        // A JSON string into a BOOL-declared tag is a genuine type mismatch.
        const auto res = client_->Post("/api/tags/Enabled", R"({"value": "not a bool"})",
                                        "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400);
    }
    {
        const auto res = client_->Post("/api/tags/Speed", "not json at all",
                                        "application/json");
        ASSERT_TRUE(res);
        EXPECT_EQ(res->status, 400);
    }
}

// Standalone (not PlcServerFixture, which always constructs a PlcServer with no
// static directory): proves a configured static directory is served at "/" without
// shadowing the /api/* routes registered alongside it.
TEST(PlcServerStaticDirTest, ServesStaticFilesAtRootWithoutShadowingApiRoutes) {
    const auto dir = std::filesystem::temp_directory_path() /
                      "softplc_plc_server_test_static";
    std::filesystem::create_directories(dir);
    {
        std::ofstream index(dir / "index.html");
        index << "<html><body>softplc web frontend</body></html>";
    }

    io::SimulatedIoDriver io;
    server::PlcServer server(io, std::chrono::milliseconds(5), dir.string());
    const int port = server.bindEphemeralPort("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread listener([&] { server.listenAfterBind(); });

    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2);
    client.set_read_timeout(2);

    const auto indexRes = client.Get("/");
    ASSERT_TRUE(indexRes);
    EXPECT_EQ(indexRes->status, 200);
    EXPECT_NE(indexRes->body.find("softplc web frontend"), std::string::npos);

    const auto statusRes = client.Get("/api/status");
    ASSERT_TRUE(statusRes);
    EXPECT_EQ(statusRes->status, 200);
    EXPECT_NE(statusRes->body.find("\"running\":false"), std::string::npos);

    server.stop();
    listener.join();
    std::filesystem::remove_all(dir);
}

TEST_F(PlcServerFixture, TagStreamEmitsAtLeastOneEvent) {
    ASSERT_TRUE(client_->Post("/api/program", kCounterProgram, "text/plain"));

    std::string received;
    const auto res = client_->Get("/api/tags/stream",
                                   [&received](const char* data, std::size_t len) {
                                       received.append(data, len);
                                       // One "data: ...\n\n" frame is enough to prove
                                       // the stream is live; stop reading so the test
                                       // doesn't block on an intentionally-endless SSE
                                       // stream.
                                       return received.find("\n\n") == std::string::npos;
                                   });
    EXPECT_NE(received.find("data: ["), std::string::npos);
    EXPECT_NE(received.find("\"name\":\"Counter\""), std::string::npos);
}
