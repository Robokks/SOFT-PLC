#include "softplc/net/serial_port.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
using handle_t = HANDLE;
constexpr handle_t kInvalidHandle = INVALID_HANDLE_VALUE;
#else
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
using handle_t = int;
constexpr handle_t kInvalidHandle = -1;
#endif

namespace softplc::net {

namespace {

handle_t toNative(std::intptr_t handle) { return static_cast<handle_t>(handle); }
std::intptr_t fromNative(handle_t h) { return static_cast<std::intptr_t>(h); }

void closeNative(handle_t h) {
#if defined(_WIN32)
    CloseHandle(h);
#else
    ::close(h);
#endif
}

enum class RawOutcome { Data, Timeout, Closed, Error };
struct RawResult {
    RawOutcome outcome;
    int bytes = 0;
};

#if defined(_WIN32)

bool configureWin32(handle_t h, const SerialConfig& config) {
    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(h, &dcb)) return false;

    dcb.BaudRate = config.baudRate;
    dcb.ByteSize = config.dataBits;
    dcb.fBinary = TRUE;
    dcb.fParity = (config.parity != SerialParity::None) ? TRUE : FALSE;
    switch (config.parity) {
        case SerialParity::None:
            dcb.Parity = NOPARITY;
            break;
        case SerialParity::Even:
            dcb.Parity = EVENPARITY;
            break;
        case SerialParity::Odd:
            dcb.Parity = ODDPARITY;
            break;
    }
    dcb.StopBits = (config.stopBits >= 2) ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;

    return SetCommState(h, &dcb) != 0;
}

void setWin32Timeouts(handle_t h, std::chrono::milliseconds timeout) {
    COMMTIMEOUTS timeouts{};
    // ReadIntervalTimeout = MAXDWORD together with zero multiplier/non-zero constant
    // means "return immediately with whatever is already buffered, otherwise wait up
    // to ReadTotalTimeoutConstant for the first byte" -- matches recvSome()'s
    // partial-result-on-timeout contract.
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = static_cast<DWORD>(timeout.count());
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = static_cast<DWORD>(timeout.count());
    SetCommTimeouts(h, &timeouts);
}

RawResult recvOnce(handle_t h, char* buf, int len, std::chrono::milliseconds timeout) {
    setWin32Timeouts(h, timeout);
    DWORD n = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(len), &n, nullptr)) return {RawOutcome::Error, 0};
    if (n == 0) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Data, static_cast<int>(n)};
}

RawResult sendOnce(handle_t h, const char* buf, int len, std::chrono::milliseconds timeout) {
    setWin32Timeouts(h, timeout);
    DWORD n = 0;
    if (!WriteFile(h, buf, static_cast<DWORD>(len), &n, nullptr)) return {RawOutcome::Error, 0};
    if (n == 0 && len > 0) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Data, static_cast<int>(n)};
}

handle_t openNative(std::string_view devicePath) {
    const std::string path(devicePath);
    return CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

#else  // POSIX

bool wasTimeout() { return errno == EAGAIN || errno == EWOULDBLOCK; }

speed_t toSpeed(std::uint32_t baudRate) {
    switch (baudRate) {
        case 1200:
            return B1200;
        case 2400:
            return B2400;
        case 4800:
            return B4800;
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        default:
            return B0;  // signals "unsupported" to the caller
    }
}

bool configurePosix(handle_t fd, const SerialConfig& config) {
    struct termios tty {};
    if (tcgetattr(fd, &tty) != 0) return false;

    const speed_t speed = toSpeed(config.baudRate);
    if (speed == B0) return false;
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    cfmakeraw(&tty);  // no line discipline, no signal chars, 8-bit clean, no output processing

    tty.c_cflag &= ~CSIZE;
    switch (config.dataBits) {
        case 5:
            tty.c_cflag |= CS5;
            break;
        case 6:
            tty.c_cflag |= CS6;
            break;
        case 7:
            tty.c_cflag |= CS7;
            break;
        case 8:
            tty.c_cflag |= CS8;
            break;
        default:
            return false;
    }

    switch (config.parity) {
        case SerialParity::None:
            tty.c_cflag &= ~PARENB;
            break;
        case SerialParity::Even:
            tty.c_cflag |= PARENB;
            tty.c_cflag &= ~PARODD;
            break;
        case SerialParity::Odd:
            tty.c_cflag |= PARENB | PARODD;
            break;
    }

    if (config.stopBits == 1) {
        tty.c_cflag &= ~CSTOPB;
    } else if (config.stopBits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        return false;
    }

    tty.c_cflag |= (CLOCAL | CREAD);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) return false;
    tcflush(fd, TCIOFLUSH);  // discard any stale bytes already buffered before this open()
    return true;
}

RawResult recvOnce(handle_t fd, char* buf, int len, std::chrono::milliseconds timeout) {
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(fd, &readSet);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);

    const int selectRc = select(fd + 1, &readSet, nullptr, nullptr, &tv);
    if (selectRc == 0) return {RawOutcome::Timeout, 0};
    if (selectRc < 0) return {RawOutcome::Error, 0};

    const int n = static_cast<int>(::read(fd, buf, static_cast<std::size_t>(len)));
    if (n > 0) return {RawOutcome::Data, n};
    if (n == 0) return {RawOutcome::Closed, 0};
    if (wasTimeout()) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Error, 0};
}

RawResult sendOnce(handle_t fd, const char* buf, int len, std::chrono::milliseconds timeout) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(fd, &writeSet);
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);

    const int selectRc = select(fd + 1, nullptr, &writeSet, nullptr, &tv);
    if (selectRc == 0) return {RawOutcome::Timeout, 0};
    if (selectRc < 0) return {RawOutcome::Error, 0};

    const int n = static_cast<int>(::write(fd, buf, static_cast<std::size_t>(len)));
    if (n >= 0) return {RawOutcome::Data, n};
    if (wasTimeout()) return {RawOutcome::Timeout, 0};
    return {RawOutcome::Error, 0};
}

handle_t openNative(std::string_view devicePath) {
    const std::string path(devicePath);
    return ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
}

#endif

}  // namespace

const char* toString(SerialError error) {
    switch (error) {
        case SerialError::None:
            return "None";
        case SerialError::OpenFailed:
            return "OpenFailed";
        case SerialError::ConfigFailed:
            return "ConfigFailed";
    }
    return "Unknown";
}

SerialPort::SerialPort() : handle_(fromNative(kInvalidHandle)) {}
SerialPort::SerialPort(std::intptr_t handle) : handle_(handle) {}
SerialPort::~SerialPort() { close(); }

SerialPort::SerialPort(SerialPort&& other) noexcept : handle_(other.handle_) {
    other.handle_ = fromNative(kInvalidHandle);
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = fromNative(kInvalidHandle);
    }
    return *this;
}

SerialPort SerialPort::adopt(std::intptr_t nativeHandle) { return SerialPort(nativeHandle); }

bool SerialPort::isOpen() const { return handle_ != fromNative(kInvalidHandle); }

void SerialPort::close() {
    if (handle_ != fromNative(kInvalidHandle)) {
        closeNative(toNative(handle_));
        handle_ = fromNative(kInvalidHandle);
    }
}

SerialError SerialPort::open(std::string_view devicePath, const SerialConfig& config) {
    close();

    const handle_t h = openNative(devicePath);
    if (h == kInvalidHandle) return SerialError::OpenFailed;

#if defined(_WIN32)
    const bool configured = configureWin32(h, config);
#else
    const bool configured = configurePosix(h, config);
#endif
    if (!configured) {
        closeNative(h);
        return SerialError::ConfigFailed;
    }

    handle_ = fromNative(h);
    return SerialError::None;
}

std::int32_t SerialPort::recvSome(std::span<std::byte> buffer, std::chrono::milliseconds timeout) {
    // Same looping contract as net::TcpSocket::recvSome() -- see that function's
    // comment for the full rationale (this is what nanoMODBUS's transport read
    // callback requires).
    if (!isOpen()) return -1;
    if (buffer.empty()) return 0;
    const handle_t h = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t received = 0;
    while (received < buffer.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result =
            recvOnce(h, reinterpret_cast<char*>(buffer.data() + received),
                     static_cast<int>(buffer.size() - received), remaining);
        switch (result.outcome) {
            case RawOutcome::Data:
                received += static_cast<std::size_t>(result.bytes);
                break;
            case RawOutcome::Timeout:
                return static_cast<std::int32_t>(received);
            case RawOutcome::Closed:
                return received > 0 ? static_cast<std::int32_t>(received) : -1;
            case RawOutcome::Error:
                return -1;
        }
    }
    return static_cast<std::int32_t>(received);
}

std::int32_t SerialPort::sendSome(std::span<const std::byte> data, std::chrono::milliseconds timeout) {
    if (!isOpen()) return -1;
    if (data.empty()) return 0;
    const handle_t h = toNative(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const RawResult result = sendOnce(h, reinterpret_cast<const char*>(data.data() + sent),
                                           static_cast<int>(data.size() - sent), remaining);
        if (result.outcome == RawOutcome::Error) return sent > 0 ? static_cast<std::int32_t>(sent) : -1;
        if (result.outcome == RawOutcome::Timeout) return static_cast<std::int32_t>(sent);
        sent += static_cast<std::size_t>(result.bytes);
    }
    return static_cast<std::int32_t>(sent);
}

}  // namespace softplc::net
