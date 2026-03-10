# fwUSBHost — portable USB host library for RP2350 + PIO-USB
#
# Usage from consuming project:
#   set(PICO_PIO_USB_PATH ...)   # before pico_sdk_init()
#   add_subdirectory(usb-host)
#   target_link_libraries(myapp PRIVATE fwUSBHost)
#   # Add TinyUSB host stack sources to your executable (see FWUSBHOST_TINYUSB_HOST_SOURCES)
#
# Provides: USB HID keyboard/mouse/controller, XInput (Xbox), CDC, MSC
# Requires: Pico SDK with PIO-USB, tusb_config.h lives in this directory
#
# NOTE: TinyUSB host stack sources (.c files from the SDK) must be compiled in
# the consuming executable, NOT in this library. This avoids duplicate symbol
# conflicts with TinyUSB's device stack (used by pico_stdio_usb for CDC serial).

# Locate TinyUSB sources from Pico SDK
if(NOT DEFINED TINYUSB_PATH)
    set(TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)
endif()

# Export the list of TinyUSB host stack sources the consuming project must compile.
# Cannot link tinyusb_host_base because it defines RP2040_USB_HOST_MODE=1 which
# forces native USB into host mode. We need native USB in device mode for CDC serial.
set(FWUSBHOST_TINYUSB_HOST_SOURCES
    ${TINYUSB_PATH}/src/host/usbh.c
    ${TINYUSB_PATH}/src/host/hub.c
    ${TINYUSB_PATH}/src/class/cdc/cdc_host.c
    ${TINYUSB_PATH}/src/class/hid/hid_host.c
    ${TINYUSB_PATH}/src/class/msc/msc_host.c
    ${TINYUSB_PATH}/src/class/vendor/vendor_host.c
    ${TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
    PARENT_SCOPE
)

add_library(fwUSBHost STATIC
    fwUSBHost.cpp
    fwUSBHostHID.cpp
    fwUSBHostHIDKeyboard.cpp
    fwUSBHostHIDMouse.cpp
    fwUSBHostHIDGeneric.cpp
    fwUSBHostHIDController.cpp
    fwUSBHostCDC.cpp
    fwUSBHostMSC.cpp
    fwUSBHostXInput.cpp
    tusb_xinput/xinput_host.c
)

# tusb_config.h lives here and must be found BEFORE SDK paths
target_include_directories(fwUSBHost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tusb_xinput
)

# TinyUSB core headers (tusb.h etc.)
target_include_directories(fwUSBHost PUBLIC
    ${TINYUSB_PATH}/src
)

target_link_libraries(fwUSBHost PUBLIC
    tinyusb_pico_pio_usb
    pico_stdlib
)

# TinyUSB debug level — override via parent project if needed
if(NOT DEFINED FWUSBHOST_TUSB_DEBUG_LEVEL)
    set(FWUSBHOST_TUSB_DEBUG_LEVEL 0)
endif()
target_compile_definitions(fwUSBHost PRIVATE
    CFG_TUSB_DEBUG=${FWUSBHOST_TUSB_DEBUG_LEVEL}
)
