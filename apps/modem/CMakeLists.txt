add_executable(weaklink-modem main.cpp)
target_link_libraries(weaklink-modem PRIVATE wl_api wl_cli11 wl_runtime wl_warnings)
target_compile_definitions(weaklink-modem PRIVATE WEAKLINK_VERSION="${WEAKLINK_VERSION}")
