include(GoogleTest)

add_executable(softplc_tests
    tags/tag_store_test.cpp
    io/simulated_io_driver_test.cpp
    io/modbus_mapping_test.cpp
    io/modbus_client_test.cpp
    io/modbus_tcp_driver_test.cpp
    core/scan_engine_test.cpp
    core/rt_scheduling_test.cpp
    net/tcp_socket_test.cpp
    st/lexer_test.cpp
    st/parser_test.cpp
    st/interpreter_test.cpp
    support/mock_modbus_server.cpp
)

target_include_directories(softplc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(softplc_tests PRIVATE softplc_core gtest_main)

gtest_discover_tests(softplc_tests)
