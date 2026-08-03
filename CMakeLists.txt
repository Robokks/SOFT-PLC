cmake_minimum_required(VERSION 3.20)
project(SoftPLC LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(SOFTPLC_BUILD_TESTS "Build unit tests" ON)
option(SOFTPLC_ENABLE_SANITIZERS "Enable AddressSanitizer/UndefinedBehaviorSanitizer" OFF)

include(cmake/CompilerWarnings.cmake)

add_library(softplc_options INTERFACE)
target_compile_features(softplc_options INTERFACE cxx_std_20)
softplc_apply_warnings(softplc_options)

if(SOFTPLC_ENABLE_SANITIZERS)
    target_compile_options(softplc_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fsanitize=address,undefined -fno-omit-frame-pointer>
        $<$<CXX_COMPILER_ID:MSVC>:/fsanitize=address>
    )
    target_link_options(softplc_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-fsanitize=address,undefined>
    )
endif()

add_subdirectory(src)
add_subdirectory(apps/plc_runner)

if(SOFTPLC_BUILD_TESTS)
    enable_testing()
    include(cmake/FetchGoogleTest.cmake)
    add_subdirectory(tests)
endif()
