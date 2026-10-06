add_executable(plc_runtime main.cpp)
target_link_libraries(plc_runtime PRIVATE softplc_core ${CMAKE_DL_LIBS})
