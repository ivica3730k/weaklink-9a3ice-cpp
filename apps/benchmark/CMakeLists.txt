add_executable(weaklink-modem-benchmark main.cpp)
target_link_libraries(weaklink-modem-benchmark PRIVATE wl_api wl_cli11 wl_runtime wl_warnings)
target_compile_definitions(weaklink-modem-benchmark PRIVATE WEAKLINK_VERSION="${WEAKLINK_VERSION}")

find_package(Threads REQUIRED)
target_link_libraries(weaklink-modem-benchmark PRIVATE Threads::Threads)
