# Generated Cmake Pico project file

# Set the minimum CMake version
cmake_minimum_required(VERSION 3.13)

# Set the C and C++ standards
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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(multirom C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(multirom 
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    multirom.c 
    nextor.c )

pico_set_program_name(multirom "multirom")
pico_set_program_version(multirom "0.1")

# point to the custom linker script
# pico_set_linker_script(multirom ${CMAKE_CURRENT_LIST_DIR}/multirom.ld)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(multirom 0)
pico_enable_stdio_usb(multirom 0)

# Add the standard library to the build
target_link_libraries(multirom 
    pico_stdlib 
    pico_multicore 
    tinyusb_board
    tinyusb_host 
    )

# Add the standard include files to the build
target_include_directories(multirom PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

# Pick Winbond boot2 (W25Qxx family, including 25Q128)
target_compile_definitions(multirom PRIVATE PICO_BOOT_STAGE2_CHOOSE_W25Q080=1)

pico_add_extra_outputs(multirom)

