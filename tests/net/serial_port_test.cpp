#include "softplc/net/serial_port.hpp"

#include <gtest/gtest.h>
#include <pty.h>
#include <unistd.h>

#include <cstring>
#include <span>

using namespace softplc::net;

namespace {

// A real pseudo-terminal pair behaves like a genuine serial line for the purposes of
// exercising SerialPort's open()/recvSome()/sendSome() -- termios configuration, raw
// mode, and byte-level timeouts all apply to the slave side exactly as they would to
// a real /dev/ttyUSB0. The test drives the master fd directly (plain read()/write(),
// no SerialPort involved) as the "other end of the wire".
struct PtyPair {
    int masterFd = -1;
    std::string slavePath;

    PtyPair() {
        int slaveFd = -1;
        char nameBuf[256];
        if (openpty(&masterFd, &slaveFd, nameBuf, nullptr, nullptr) != 0) {
            ADD_FAILURE() << "openpty failed: " << std::strerror(errno);
            return;
        }
        slavePath = nameBuf;
        ::close(slaveFd);  // SerialPort::open() reopens it by path
    }

    ~PtyPair() {
        if (masterFd >= 0) ::close(masterFd);
    }
};

SerialConfig defaultConfig() {
    return SerialConfig{.baudRate = 9600, .dataBits = 8, .parity = SerialParity::None, .stopBits = 1};
}

}  // namespace

TEST(SerialPortTest, OpenFailsForNonexistentDevicePath) {
    SerialPort port;
    EXPECT_EQ(port.open("/dev/this-does-not-exist-softplc-test", defaultConfig()), SerialError::OpenFailed);
    EXPECT_FALSE(port.isOpen());
}

TEST(SerialPortTest, OpenFailsForUnsupportedBaudRate) {
    PtyPair pty;
    SerialConfig config = defaultConfig();
    config.baudRate = 123456;  // not one of the standard termios-mappable rates
    SerialPort port;
    EXPECT_EQ(port.open(pty.slavePath, config), SerialError::ConfigFailed);
}

TEST(SerialPortTest, OpenFailsForUnsupportedDataBits) {
    PtyPair pty;
    SerialConfig config = defaultConfig();
    config.dataBits = 4;
    SerialPort port;
    EXPECT_EQ(port.open(pty.slavePath, config), SerialError::ConfigFailed);
}

TEST(SerialPortTest, BytesWrittenToMasterAreReadableViaRecvSome) {
    PtyPair pty;
    SerialPort port;
    ASSERT_EQ(port.open(pty.slavePath, defaultConfig()), SerialError::None);

    const char message[] = "hello";
    ASSERT_EQ(::write(pty.masterFd, message, 5), 5);

    std::byte buffer[5];
    const std::int32_t n = port.recvSome(std::span<std::byte>(buffer, 5), std::chrono::milliseconds(500));
    ASSERT_EQ(n, 5);
    EXPECT_EQ(std::memcmp(buffer, message, 5), 0);
}

TEST(SerialPortTest, BytesSentViaSendSomeAreReadableFromMaster) {
    PtyPair pty;
    SerialPort port;
    ASSERT_EQ(port.open(pty.slavePath, defaultConfig()), SerialError::None);

    const std::byte outgoing[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    const std::int32_t sent =
        port.sendSome(std::span<const std::byte>(outgoing, 3), std::chrono::milliseconds(500));
    ASSERT_EQ(sent, 3);

    char received[3] = {};
    ASSERT_EQ(::read(pty.masterFd, received, 3), 3);
    EXPECT_EQ(received[0], 0x01);
    EXPECT_EQ(received[1], 0x02);
    EXPECT_EQ(received[2], 0x03);
}

TEST(SerialPortTest, RecvSomeReturnsZeroOnTimeoutWithNoDataAvailable) {
    PtyPair pty;
    SerialPort port;
    ASSERT_EQ(port.open(pty.slavePath, defaultConfig()), SerialError::None);

    std::byte buffer[8];
    const std::int32_t n = port.recvSome(std::span<std::byte>(buffer, 8), std::chrono::milliseconds(50));
    // Timeout with zero bytes transferred is a valid, non-error partial result --
    // matches net::TcpSocket::recvSome()'s contract exactly.
    EXPECT_EQ(n, 0);
}

TEST(SerialPortTest, RecvSomeOnUnopenedPortReturnsHardError) {
    SerialPort port;
    std::byte buffer[4];
    EXPECT_EQ(port.recvSome(std::span<std::byte>(buffer, 4), std::chrono::milliseconds(50)), -1);
}
