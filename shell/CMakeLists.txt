cmake_minimum_required(VERSION 3.18)
project(luna-shell VERSION 0.1.0 LANGUAGES C)
set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD 11)
add_compile_options(-arch arm64 -Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)

foreach(PROTO wlr-layer-shell-unstable-v1 wlr-foreign-toplevel-management-unstable-v1)
    add_custom_command(
        OUTPUT ${PROTO}-client-protocol.h ${PROTO}-protocol.c
        COMMAND wayland-scanner client-header
                ${CMAKE_SOURCE_DIR}/protocols/${PROTO}.xml ${PROTO}-client-protocol.h
        COMMAND wayland-scanner private-code
                ${CMAKE_SOURCE_DIR}/protocols/${PROTO}.xml ${PROTO}-protocol.c)
endforeach()

add_executable(luna-shell
    src/luna-shell-main.c
    src/luna-panel.c
    src/luna-launcher-wallpaper.c
    wlr-layer-shell-unstable-v1-protocol.c
    wlr-foreign-toplevel-management-unstable-v1-protocol.c)
target_include_directories(luna-shell PRIVATE
    include ${CMAKE_BINARY_DIR} ${WAYLAND_CLIENT_INCLUDE_DIRS})
target_link_libraries(luna-shell ${WAYLAND_CLIENT_LIBRARIES} m)
install(TARGETS luna-shell RUNTIME DESTINATION bin)
