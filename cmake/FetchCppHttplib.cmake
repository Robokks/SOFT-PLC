include(FetchContent)

# Header-only; explicitly disable optional TLS/compression backends so this stays a
# plain-HTTP, zero-extra-runtime-dependency build like the rest of this project (no
# implicit OpenSSL/zlib/brotli link just because a build machine happens to have them
# installed). The programming/monitoring API is meant for a trusted local network, same
# threat model as this project's existing Modbus TCP/RTU drivers.
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_BROTLI OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
)
FetchContent_MakeAvailable(httplib)
