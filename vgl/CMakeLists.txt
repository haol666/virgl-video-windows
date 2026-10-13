cmake_minimum_required(VERSION 3.20)
project(vgl C)

# libvgl - virtio-gpu video codec client library (Windows guest).
# Compiles on any Windows runner without WDK (transport uses plain Win32).

set(CMAKE_C_STANDARD 11)

add_library(vgl STATIC
    src/vgl_session.c
    src/vgl_resource.c
    src/vgl_cmd.c
    src/vgl_transport_win.c
)

target_include_directories(vgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(vgl PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(vgl PRIVATE /W4)
else()
    target_compile_options(vgl PRIVATE -Wall -Wextra)
endif()

enable_testing()

add_executable(test_wire tests/test_wire.c)
target_link_libraries(test_wire PRIVATE vgl)
target_compile_definitions(test_wire PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(test_wire PRIVATE /W4)
endif()

add_test(NAME wire COMMAND test_wire)
