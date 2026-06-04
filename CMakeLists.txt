cmake_minimum_required(VERSION 3.22)
project(WaitingServer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(WaitingServer src/main.cpp)

target_include_directories(WaitingServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)