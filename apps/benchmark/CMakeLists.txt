add_executable(weaklink-modem-benchmark main.cpp)
target_link_libraries(weaklink-modem-benchmark PRIVATE wl_api wl_cli11 wl_warnings)
find_package(Threads REQUIRED)
target_link_libraries(weaklink-modem-benchmark PRIVATE Threads::Threads)
