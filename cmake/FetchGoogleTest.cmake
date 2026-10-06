include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
if(MSVC)
    # Match the application's /MD[d] runtime instead of Google's default /MT[d].
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
FetchContent_MakeAvailable(googletest)
