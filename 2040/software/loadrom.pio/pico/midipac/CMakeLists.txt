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

project(midipac C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Reuse the USB MIDI host driver from the midi firmware
set(MIDI_DIR ${CMAKE_CURRENT_LIST_DIR}/../midi)

add_executable(midipac
    ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c
    ${MIDI_DIR}/usb_midi_host.c
    midipac_main.c)

# PIO program for passive PSG write interception
pico_generate_pio_header(midipac ${CMAKE_CURRENT_LIST_DIR}/msx_midipac.pio)

pico_set_program_name(midipac "midipac")
pico_set_program_version(midipac "1.0")

# No stdio — this firmware is headless
pico_enable_stdio_uart(midipac 0)
pico_enable_stdio_usb(midipac 0)

target_link_libraries(midipac
        pico_stdlib
        pico_multicore
        hardware_pio
        hardware_timer
        tinyusb_board
        tinyusb_host)

target_include_directories(midipac PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}
  ${MIDI_DIR}
)

pico_add_extra_outputs(midipac)

# Copy firmware binary to dist folder for the tool to embed
option(PICOVERSE_COPY_TO_DIST "Copy built firmware into the legacy dist directory" ON)
if(PICOVERSE_COPY_TO_DIST)
add_custom_command(TARGET midipac POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/midipac.bin ${CMAKE_SOURCE_DIR}/dist/midipac.bin
)
endif()
