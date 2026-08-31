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
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_COPY_TO_RAM 0)

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(explorer C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Set to 1 to enable USB stdio debug output, 0 to disable. If enabled, the pico will appear as a USB CDC device when connected to a host, and printf output will be sent over USB instead of UART. Note that enabling this will prevent the use of the USB host stack, so it is not recommended for normal use.
# I used to debug some pico crashes during development...
# This also builds the SCC / PSG-mirror bus tracer (debug/audio_trace.c), which
# records MSX I/O writes, PSG register traffic, SCC register writes and the PIO
# FIFO stall flags into a per-core ring buffer and reports over the same CDC link.
set(EXPLORER_USB_STDIO_DEBUG 0)

# Add executable. Default name is the project name, version 0.1
set(EXPLORER_SOURCES
    storage/hw_config.c
    storage/sunrise_ide.c
    storage/sunrise_sd.c
    memory/c2_emu.c
    explorer.c 
    audio/mp3.c
    audio/emu2212.c
    audio/emu2149.c
    audio/emu2413.c
    audio/emu2151.cpp
    audio/ymfm/ymfm_opm.cpp
    debug/audio_trace.c
    msx_bus.pio
)

if (NOT EXPLORER_USB_STDIO_DEBUG)
    list(APPEND EXPLORER_SOURCES ${PICO_SDK_PATH}/lib/tinyusb/src/tusb.c)
endif()

add_executable(explorer ${EXPLORER_SOURCES})

pico_generate_pio_header(explorer ${CMAKE_CURRENT_SOURCE_DIR}/msx_bus.pio)

add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src build)
add_subdirectory(lib/picomp3lib/src picomp3lib_build)

pico_set_program_name(explorer "explorer")
pico_set_program_version(explorer "${EXPLORER_VERSION}")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(explorer 0)
pico_enable_stdio_usb(explorer ${EXPLORER_USB_STDIO_DEBUG})

# Add the standard library to the build
target_link_libraries(explorer
        pico_stdlib
    pico_unique_id
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
        pico_multicore
    pico_audio_i2s
    picomp3lib
    hardware_dma
    hardware_pio
    tinyusb_board
        )

if (NOT EXPLORER_USB_STDIO_DEBUG)
    target_link_libraries(explorer tinyusb_host)
endif()

target_compile_definitions(explorer PRIVATE
    PICO_AUDIO_I2S_PIO=1
    EXPLORER_USB_STDIO_DEBUG=$<BOOL:${EXPLORER_USB_STDIO_DEBUG}>
    EXPLORER_AUDIO_TRACE=$<BOOL:${EXPLORER_USB_STDIO_DEBUG}>
    EXPLORER_VERSION="${EXPLORER_VERSION}"
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=5000
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000
    USBD_PID=0x00E1
    USBD_PRODUCT="PicoVerse USB Debug CDC"
)

# Add the standard include files to the build
target_include_directories(explorer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(explorer)
