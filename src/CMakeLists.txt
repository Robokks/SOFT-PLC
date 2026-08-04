add_library(softplc_core STATIC
    tags/value.cpp
    tags/tag_store.cpp
    io/simulated_io_driver.cpp
    io/modbus_mapping.cpp
    io/modbus_client.cpp
    io/modbus_tcp_driver.cpp
    io/modbus_rtu_bus.cpp
    io/modbus_rtu_driver.cpp
    core/scan_engine.cpp
    core/rt_scheduling.cpp
    net/tcp_socket.cpp
    net/tcp_listener.cpp
    net/serial_port.cpp
    st/lexer.cpp
    st/parser.cpp
    st/pou_binder.cpp
    st/interpreter.cpp
    st/st_program.cpp
    server/plc_server.cpp
)

target_include_directories(softplc_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(softplc_core PUBLIC softplc_options)

find_package(Threads REQUIRED)
target_link_libraries(softplc_core PUBLIC Threads::Threads)

target_link_libraries(softplc_core PUBLIC nanomodbus)
target_link_libraries(softplc_core PUBLIC httplib::httplib)

if(WIN32)
    target_link_libraries(softplc_core PUBLIC ws2_32)
endif()
