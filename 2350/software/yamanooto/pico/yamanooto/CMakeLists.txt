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
# Core2350B / PGA2350 uses an RP2350B (48 GPIOs). The 'pico2' board is RP2350A
# (30 GPIOs only) and silently ignores any access to GPIO30..47. Use an
# RP2350B board definition so NUM_BANK0_GPIOS is 48.
set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

# Fetch pico-extras if not available locally (provides pico_audio_i2s)
set(PICO_EXTRAS_FETCH_FROM_GIT ON)
include(pico_extras_import.cmake)

project(yamanooto C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Set to 1 to enable USB CDC stdio debug output, matching the Explorer debug
# build style. The cartridge appears as a USB serial device and debug_trace()
# output is sent over USB.
set(YAMANOOTO_USB_STDIO_DEBUG 0)

# Add executable. Default name is the project name, version 0.1
add_executable(yamanooto
    yamanooto.c
    emu2212.c
    emu2149.c
    emu2413.c
    )

pico_generate_pio_header(yamanooto ${CMAKE_CURRENT_LIST_DIR}/msx_bus.pio)

pico_set_program_name(yamanooto "yamanooto")
pico_set_program_version(yamanooto "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(yamanooto 0)
pico_enable_stdio_usb(yamanooto ${YAMANOOTO_USB_STDIO_DEBUG})

# Add the standard library to the build
target_link_libraries(yamanooto
    pico_stdlib
    pico_multicore
    pico_audio_i2s
    hardware_dma
    hardware_flash
    hardware_pio)

target_compile_definitions(yamanooto PRIVATE
    PICO_AUDIO_I2S_PIO=0
    YAMANOOTO_USB_STDIO_DEBUG=$<BOOL:${YAMANOOTO_USB_STDIO_DEBUG}>
    YAMANOOTO_FMVOICE_TRACE=0
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000
    USBD_PID=0x00E2
    USBD_PRODUCT="PicoVerse Yamanooto USB Debug CDC"
)

# Add the standard include files to the build
target_include_directories(yamanooto PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(yamanooto)

# Custom command to copy files to dist folder
option(PICOVERSE_COPY_TO_DIST "Copy built firmware into the legacy dist directory" ON)
if(PICOVERSE_COPY_TO_DIST)
add_custom_command(TARGET yamanooto POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/yamanooto.bin ${CMAKE_SOURCE_DIR}/dist/yamanooto.bin
)
endif()
