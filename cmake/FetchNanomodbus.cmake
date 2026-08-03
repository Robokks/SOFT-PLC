include(FetchContent)

FetchContent_Declare(
    nanomodbus
    GIT_REPOSITORY https://github.com/debevv/nanoMODBUS.git
    GIT_TAG v1.23.0
)
FetchContent_MakeAvailable(nanomodbus)
