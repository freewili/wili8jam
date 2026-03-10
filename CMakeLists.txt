cmake_minimum_required(VERSION 3.30)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PICO_PLATFORM rp2350)
set(PICO_BOARD adafruit_fruit_jam CACHE STRING "Board type")

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()

# Pico-PIO-USB path must be set BEFORE pico_sdk_init() so TinyUSB creates the target
set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Pico-PIO-USB)

include(pico_sdk_import.cmake)

project(wili8jam C CXX ASM)

pico_sdk_init()

# --- Lua 5.4.7 static library ---
set(LUA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lua-5.4.7/src)

file(GLOB LUA_CORE_SOURCES ${LUA_SRC_DIR}/*.c)
# Remove standalone program mains
list(REMOVE_ITEM LUA_CORE_SOURCES
    ${LUA_SRC_DIR}/lua.c
    ${LUA_SRC_DIR}/luac.c
)

add_library(lua54 STATIC ${LUA_CORE_SOURCES})

target_include_directories(lua54 PUBLIC ${LUA_SRC_DIR})

# luaconf overrides for embedded use (luaconf.h has #ifndef guards for these)
target_compile_definitions(lua54 PUBLIC
    LUA_32BITS=1
    LUAI_MAXSTACK=2000
)
target_compile_definitions(lua54 PRIVATE
    LUA_USE_C89
)

# Ensure Lua is built as C
set_target_properties(lua54 PROPERTIES LINKER_LANGUAGE C)

# --- TLSF allocator (vendored) ---
add_library(tlsf STATIC tlsf/tlsf.c)
target_include_directories(tlsf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- FatFS library (SD card) ---
add_library(fatfs STATIC
    fatfs/ff.c
    fatfs/ffunicode.c
    fatfs/ffsystem.c
    fatfs/diskio.c
)
target_include_directories(fatfs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/fatfs
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# --- USB Host library (PIO-USB, HID, XInput, CDC, MSC) ---
set(FWUSBHOST_TUSB_DEBUG_LEVEL 2)  # output goes to ring buffer, dumped via 'info'
add_subdirectory(usb-host)

# --- Main executable ---
add_executable(wili8jam
    src/main.cpp
    src/sdcard.c
    src/dvi.c
    src/gfx.c
    src/input.c
    src/audio.c
    src/p8_preprocess.c
    src/p8_api.c
    src/p8_sfx.c
    src/p8_cart.c
    src/p8_png.c
    src/p8_console.c
    src/p8_editor.c
)

# Generate PIO header for I2S audio
pico_generate_pio_header(wili8jam ${CMAKE_CURRENT_SOURCE_DIR}/src/audio_i2s.pio)

# usb-host/ and tusb_xinput/ include dirs come from fwUSBHost target
target_include_directories(wili8jam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LUA_SRC_DIR}
)

target_link_libraries(wili8jam PRIVATE
    lua54
    tlsf
    fatfs
    fwUSBHost
    pico_stdlib
    hardware_pio
    hardware_spi
    hardware_sync
    hardware_dma
    hardware_clocks
    hardware_i2c
)

# Increase stack size — default 0x800 (2KB) is too small for Lua + FatFS
# Lua call chain can be deep; FIL structs now static to save stack space
target_compile_definitions(wili8jam PRIVATE PICO_STACK_SIZE=0x1000)

# SDK's stdio_usb handles CDC serial (device descriptors, tud_task timer, etc.)
pico_enable_stdio_usb(wili8jam 1)
pico_enable_stdio_uart(wili8jam 0)

pico_add_extra_outputs(wili8jam)
