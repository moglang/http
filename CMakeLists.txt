cmake_minimum_required(VERSION 3.16)
project(mog_http LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

set(USOCKETS_SOURCES
    vendor/uSockets/src/bsd.c
    vendor/uSockets/src/context.c
    vendor/uSockets/src/loop.c
    vendor/uSockets/src/socket.c
    vendor/uSockets/src/udp.c
    vendor/uSockets/src/eventing/epoll_kqueue.c
)

add_library(usockets STATIC ${USOCKETS_SOURCES})
set_target_properties(usockets PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(usockets PUBLIC vendor/uSockets/src)
target_compile_definitions(usockets PUBLIC LIBUS_NO_SSL)
target_link_libraries(usockets PUBLIC Threads::Threads)

add_library(mog_http MODULE
    src/package.cpp
    src/host_bridge.cpp
    src/server.cpp
    src/http.cpp
    src/validation.cpp
)
target_include_directories(mog_http PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/uWebSockets/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/uSockets/src"
)
target_compile_definitions(mog_http PRIVATE
    LIBUS_NO_SSL
    UWS_NO_ZLIB
)
target_link_libraries(mog_http PRIVATE usockets Threads::Threads)
set_target_properties(mog_http PROPERTIES PREFIX "" OUTPUT_NAME "package")
