cmake_minimum_required(VERSION 3.18)
project(luna-compositor VERSION 0.1.0 LANGUAGES C)

set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
set(CMAKE_C_STANDARD 11)
add_compile_options(-arch arm64 -Wall -Wno-unused-parameter -DWLR_USE_UNSTABLE)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS   REQUIRED wlroots)
pkg_check_modules(WAYLAND   REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

add_executable(luna-compositor src/main.c)
target_include_directories(luna-compositor PRIVATE
    include
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
)
target_link_libraries(luna-compositor
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
)
install(TARGETS luna-compositor RUNTIME DESTINATION bin)
