cmake_minimum_required(VERSION 3.20)
project(WaitingServerLinux CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

add_library(asio_interface INTERFACE)
target_include_directories(asio_interface INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_interface INTERFACE ASIO_STANDALONE)

add_executable(waiting_server
    src/main.cpp
)

target_include_directories(waiting_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)

target_link_libraries(waiting_server PRIVATE
    asio_interface
    pthread
)
