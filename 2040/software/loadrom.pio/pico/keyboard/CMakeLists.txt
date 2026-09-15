# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 13_3_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 13_3_Rel1)
set(picotoolVersion 2.1.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(keyboard C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(keyboard
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    keyboard_main.c)

# Dedicated PIO programs for keyboard I/O bus handling (with /WAIT support)
pico_generate_pio_header(keyboard ${CMAKE_CURRENT_LIST_DIR}/msx_keyboard.pio)

pico_set_program_name(keyboard "keyboard")
pico_set_program_version(keyboard "1.0")

# No stdio — this firmware is headless
pico_enable_stdio_uart(keyboard 0)
pico_enable_stdio_usb(keyboard 0)

target_link_libraries(keyboard
        pico_stdlib
        pico_multicore
        hardware_pio
        tinyusb_board
        tinyusb_host)

target_include_directories(keyboard PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(keyboard)

# Copy firmware binary to dist folder for the tool to embed
option(PICOVERSE_COPY_TO_DIST "Copy built firmware into the legacy dist directory" ON)
if(PICOVERSE_COPY_TO_DIST)
add_custom_command(TARGET keyboard POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/keyboard.bin ${CMAKE_SOURCE_DIR}/dist/keyboard.bin
)
endif()
