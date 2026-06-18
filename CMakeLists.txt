cmake_minimum_required(VERSION 3.22)
project(WaitingServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Asio standalone header-only library
include(FetchContent)
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG asio-1-30-2
)
FetchContent_MakeAvailable(asio)

add_executable(WaitingServer src/main.cpp)

# Define Asio standalone target manually since the upstream repo CMake lists
# are geared towards autotools/installed packages and lack export targets.
add_library(asio_interface INTERFACE)
target_include_directories(asio_interface INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio_interface INTERFACE ASIO_STANDALONE)

target_link_libraries(WaitingServer PRIVATE asio_interface)
target_include_directories(WaitingServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)