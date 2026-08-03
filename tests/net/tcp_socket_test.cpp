#include "softplc/net/tcp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <thread>

#include "softplc/net/tcp_listener.hpp"

using namespace softplc::net;

TEST(TcpSocketTest, ConnectSendRecvRoundTrip) {
    TcpListener listener;
    ASSERT_TRUE(listener.listen(0));
    const std::uint16_t port = listener.boundPort();
    ASSERT_NE(port, 0);

    std::thread serverThread([&listener] {
        auto server = listener.accept(std::chrono::milliseconds(2000));
        ASSERT_TRUE(server.has_value());
        std::array<std::byte, 5> buf{};
        const auto recvErr = server->recvExact(buf, std::chrono::milliseconds(2000));
        EXPECT_EQ(recvErr, SocketError::None);
        const auto sendErr = server->send(buf, std::chrono::milliseconds(2000));
        EXPECT_EQ(sendErr, SocketError::None);
    });

    TcpSocket client;
    ASSERT_EQ(client.connect("127.0.0.1", port, std::chrono::milliseconds(2000)), SocketError::None);

    const std::array<std::byte, 5> outData = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                               std::byte{5}};
    ASSERT_EQ(client.send(outData, std::chrono::milliseconds(2000)), SocketError::None);

    std::array<std::byte, 5> inData{};
    ASSERT_EQ(client.recvExact(inData, std::chrono::milliseconds(2000)), SocketError::None);
    EXPECT_EQ(inData, outData);

    serverThread.join();
}

TEST(TcpSocketTest, RecvTimesOutWhenNoDataArrives) {
    TcpListener listener;
    ASSERT_TRUE(listener.listen(0));
    const std::uint16_t port = listener.boundPort();

    std::thread serverThread([&listener] {
        auto server = listener.accept(std::chrono::milliseconds(2000));
        ASSERT_TRUE(server.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    });

    TcpSocket client;
    ASSERT_EQ(client.connect("127.0.0.1", port, std::chrono::milliseconds(2000)), SocketError::None);

    std::array<std::byte, 5> buf{};
    const auto err = client.recvExact(buf, std::chrono::milliseconds(50));
    EXPECT_EQ(err, SocketError::RecvTimeout);

    serverThread.join();
}

TEST(TcpSocketTest, ConnectRefusedOnClosedPort) {
    TcpListener listener;
    ASSERT_TRUE(listener.listen(0));
    const std::uint16_t port = listener.boundPort();
    listener.close();  // nothing listens on this port anymore

    TcpSocket client;
    const auto err = client.connect("127.0.0.1", port, std::chrono::milliseconds(1000));
    EXPECT_NE(err, SocketError::None);
}

TEST(TcpSocketTest, DefaultConstructedSocketIsNotOpen) {
    TcpSocket socket;
    EXPECT_FALSE(socket.isOpen());
}

TEST(TcpSocketTest, RecvExactOnUnconnectedSocketReturnsNotConnected) {
    TcpSocket socket;
    std::array<std::byte, 4> buf{};
    EXPECT_EQ(socket.recvExact(buf, std::chrono::milliseconds(50)), SocketError::NotConnected);
}
