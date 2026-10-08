# One interface target carrying the project's warning policy; every
# weaklink library links it so the flags can't drift between targets.
add_library(wl_warnings INTERFACE)

if(MSVC)
  target_compile_options(wl_warnings INTERFACE /W4 /permissive-)
else()
  target_compile_options(wl_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
endif()
