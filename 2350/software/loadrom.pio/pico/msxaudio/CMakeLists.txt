# Generated Cmake Pico project file - standalone MSX-AUDIO (Y8950) firmware

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
set(sdkVersion 2.3.1)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# RP2350B board with external PSRAM (matches the other loadrom firmwares).
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally (provides pico_audio_i2s)
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(msxaudio C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

set(MSXAUDIO_TARGET_NAME msxaudio CACHE STRING "Standalone MSX-AUDIO firmware target/output name")

# Set to 1 to enable USB CDC debug output, 0 to disable. Keep this off for
# normal timing validation; enabling it creates a USB CDC device and routes the
# MSX-AUDIO diagnostics over USB stdio for field captures.
set(MSXAUDIO_USB_STDIO_DEBUG 0)

add_executable(${MSXAUDIO_TARGET_NAME}
    msxaudio_main.c
    emuy8950.cpp
    ymfm/ymfm_opl.cpp
    ymfm/ymfm_adpcm.cpp
    ymfm/ymfm_pcm.cpp
)

# The Y8950 synthesis (ymfm plus the emuy8950 wrapper) shares Core 0 with the
# MSX bus responder, so every microsecond it saves is a microsecond less that a
# Z80 bus cycle can be stretched. Raise just these translation units from the
# SDK default -O2 to -O3; this is optimization only and does not change the
# emulated behaviour.
set_source_files_properties(
    emuy8950.cpp
    ymfm/ymfm_opl.cpp
    ymfm/ymfm_adpcm.cpp
    ymfm/ymfm_pcm.cpp
    PROPERTIES COMPILE_OPTIONS "-O3;-funroll-loops"
)

# PIO programs for the MSX bus (unified read responder, write captors, /BUSDIR)
pico_generate_pio_header(${MSXAUDIO_TARGET_NAME} ${CMAKE_CURRENT_LIST_DIR}/msx_msxaudio.pio)

pico_set_program_name(${MSXAUDIO_TARGET_NAME} "msxaudio")
pico_set_program_version(${MSXAUDIO_TARGET_NAME} "1.0")

# Run the whole firmware from SRAM. Core 0 interleaves MSX bus responses with
# Y8950 synthesis, so no code path may ever wait on an XIP cache miss.
pico_set_binary_type(${MSXAUDIO_TARGET_NAME} copy_to_ram)

pico_enable_stdio_uart(${MSXAUDIO_TARGET_NAME} 0)
pico_enable_stdio_usb(${MSXAUDIO_TARGET_NAME} ${MSXAUDIO_USB_STDIO_DEBUG})

target_link_libraries(${MSXAUDIO_TARGET_NAME}
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_dma
    hardware_pio
)

if (MSXAUDIO_USB_STDIO_DEBUG)
    target_link_libraries(${MSXAUDIO_TARGET_NAME} tinyusb_board)
endif()

target_compile_definitions(${MSXAUDIO_TARGET_NAME} PRIVATE
    PICO_AUDIO_I2S_PIO=1
    MSXAUDIO_USB_DEBUG=$<BOOL:${MSXAUDIO_USB_STDIO_DEBUG}>
)

if (MSXAUDIO_USB_STDIO_DEBUG)
    target_compile_definitions(${MSXAUDIO_TARGET_NAME} PRIVATE
        PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000
        PICO_STDIO_USB_POST_CONNECT_WAIT_DELAY_MS=500
        PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000
        USBD_PID=0x00E1
        USBD_PRODUCT="PicoVerse USB Debug CDC"
    )
endif()

target_include_directories(${MSXAUDIO_TARGET_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(${MSXAUDIO_TARGET_NAME})

# Copy firmware binary to dist folder for the tool to embed
add_custom_command(TARGET ${MSXAUDIO_TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/${MSXAUDIO_TARGET_NAME}.bin ${CMAKE_SOURCE_DIR}/dist/${MSXAUDIO_TARGET_NAME}.bin
)
