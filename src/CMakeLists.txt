add_library(softplc_core STATIC
    tags/value.cpp
    tags/tag_store.cpp
    io/simulated_io_driver.cpp
    core/scan_engine.cpp
    st/lexer.cpp
    st/parser.cpp
    st/interpreter.cpp
    st/st_program.cpp
)

target_include_directories(softplc_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(softplc_core PUBLIC softplc_options)

find_package(Threads REQUIRED)
target_link_libraries(softplc_core PUBLIC Threads::Threads)
