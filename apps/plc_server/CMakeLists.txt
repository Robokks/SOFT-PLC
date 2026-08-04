add_executable(plc_server main.cpp)
target_link_libraries(plc_server PRIVATE softplc_core)
