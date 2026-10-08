cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(waiting_server C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(waiting_server waiting_server.cpp )

pico_set_program_name(waiting_server "waiting_server")
pico_set_program_version(waiting_server "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(waiting_server 0)
pico_enable_stdio_usb(waiting_server 1)

# Add the standard library to the build
target_link_libraries(waiting_server
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(waiting_server PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_CURRENT_LIST_DIR}/../shared/include
)

# Add any user requested libraries
target_link_libraries(waiting_server 
    pico_stdlib
    pico_cyw43_arch_lwip_poll
)

pico_add_extra_outputs(waiting_server)

# Automatically generate waiting_server_configured.uf2 on every build
add_custom_command(TARGET waiting_server POST_BUILD
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../tools/make_config.py -c ${CMAKE_CURRENT_LIST_DIR}/config.txt -o ${CMAKE_CURRENT_BINARY_DIR}/config.uf2
    COMMAND cat ${CMAKE_CURRENT_BINARY_DIR}/waiting_server.uf2 ${CMAKE_CURRENT_BINARY_DIR}/config.uf2 > ${CMAKE_CURRENT_BINARY_DIR}/waiting_server_configured.uf2
    COMMENT "Generating waiting_server_configured.uf2 with flash configuration sector"
)

