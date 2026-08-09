# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico-opm-writer C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(pico-opm-writer pico-opm-writer.c opm.c )

pico_set_program_name(pico-opm-writer "pico-opm-writer")
pico_set_program_version(pico-opm-writer "0.1")

# Generate PIO header
pico_generate_pio_header(pico-opm-writer ${CMAKE_CURRENT_LIST_DIR}/opm_clock.pio)

# φM プリセット（0 = 4MHz / 1 = 3.579545MHz）。未指定なら opm.h の既定値を使う。
set(OPM_CLOCK_MODE "" CACHE STRING "phiM preset: 0 = 4MHz, 1 = 3.579545MHz")
if(NOT OPM_CLOCK_MODE STREQUAL "")
    target_compile_definitions(pico-opm-writer PRIVATE OPM_CLOCK_MODE=${OPM_CLOCK_MODE})
endif()

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico-opm-writer 0)
pico_enable_stdio_usb(pico-opm-writer 1)

# Add the standard library to the build
target_link_libraries(pico-opm-writer
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(pico-opm-writer PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(pico-opm-writer
        hardware_pio
        hardware_clocks
        )

pico_add_extra_outputs(pico-opm-writer)

