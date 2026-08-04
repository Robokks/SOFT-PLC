#include "softplc/server/plc_server.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
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
