# fwUSBHost — portable USB host library for RP2350 + PIO-USB
#
# Usage from consuming project:
#   set(PICO_PIO_USB_PATH ...)   # before pico_sdk_init()
#   add_subdirectory(usb-host)
#   target_link_libraries(myapp PRIVATE fwUSBHost)
#
# Provides: USB HID keyboard/mouse/controller, XInput (Xbox), CDC, MSC
# Requires: Pico SDK with PIO-USB, tusb_config.h lives in this directory

# Locate TinyUSB sources from Pico SDK
if(NOT DEFINED TINYUSB_PATH)
    set(TINYUSB_PATH ${PICO_SDK_PATH}/lib/tinyusb)
endif()

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
    # TinyUSB host stack (no native HCD — PIO-USB only)
    # We cannot link tinyusb_host_base because it defines RP2040_USB_HOST_MODE=1
    # which configures native USB hardware for host mode. We need native USB in
    # device mode for CDC serial, so include host stack sources directly.
    ${TINYUSB_PATH}/src/host/usbh.c
    ${TINYUSB_PATH}/src/host/hub.c
    ${TINYUSB_PATH}/src/class/cdc/cdc_host.c
    ${TINYUSB_PATH}/src/class/hid/hid_host.c
    ${TINYUSB_PATH}/src/class/msc/msc_host.c
    ${TINYUSB_PATH}/src/class/vendor/vendor_host.c
    # PIO-USB HCD
    ${TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

# tusb_config.h lives here and must be found BEFORE SDK paths
target_include_directories(fwUSBHost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/tusb_xinput
)

# TinyUSB headers (PRIVATE — consumers don't need raw TinyUSB paths)
target_include_directories(fwUSBHost PRIVATE
    ${TINYUSB_PATH}/src
)

target_link_libraries(fwUSBHost PUBLIC
    tinyusb_pico_pio_usb
)

# TinyUSB debug level — override via parent project if needed
if(NOT DEFINED FWUSBHOST_TUSB_DEBUG_LEVEL)
    set(FWUSBHOST_TUSB_DEBUG_LEVEL 0)
endif()
target_compile_definitions(fwUSBHost PUBLIC
    CFG_TUSB_DEBUG=${FWUSBHOST_TUSB_DEBUG_LEVEL}
)
