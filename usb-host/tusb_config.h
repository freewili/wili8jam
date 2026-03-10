/*
 * TinyUSB Configuration for Adafruit Fruit Jam (wili8jam)
 *
 * Device side: Replicates SDK's pico_stdio_usb tusb_config.h exactly
 * so that CDC serial, vendor reset interface, and MS OS 2.0 descriptors
 * all work as the SDK expects.
 *
 * Host side: Adds PIO-USB host on port 1 (GPIO1/2) for keyboard input.
 */

#ifndef _PICO_STDIO_USB_TUSB_CONFIG_H
#define _PICO_STDIO_USB_TUSB_CONFIG_H

/* ---- TinyUSB debug logging → ring buffer, shown via info command ---- */
#ifdef __cplusplus
extern "C" {
#endif
extern int tusb_debug_buffered_printf(const char *fmt, ...);
#ifdef __cplusplus
}
#endif
#define CFG_TUSB_DEBUG_PRINTF   tusb_debug_buffered_printf

/* ---- Device side: identical to SDK's pico_stdio_usb/include/tusb_config.h ---- */

#include "pico/stdio_usb.h"

#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)

#define CFG_TUD_CDC             (1)

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#endif

#if !PICO_STDIO_USB_RESET_INTERFACE_SUPPORT_MS_OS_20_DESCRIPTOR
#define CFG_TUD_VENDOR            (0)
#else
#define CFG_TUD_VENDOR            (1)
#define CFG_TUD_VENDOR_RX_BUFSIZE  (256)
#define CFG_TUD_VENDOR_TX_BUFSIZE  (256)
#endif
#endif

/* ---- Host side: PIO-USB on port 1 ---- */

#define CFG_TUH_ENABLED       1
#define CFG_TUH_RPI_PIO_USB   1
#define BOARD_TUH_RHPORT      1
#define CFG_TUH_MAX_SPEED     OPT_MODE_FULL_SPEED
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB                 1
#define CFG_TUH_CDC                 2
#define CFG_TUH_CDC_FTDI            0
#define CFG_TUH_CDC_CP210X          0
#define CFG_TUH_CDC_CH34X           1
#define CFG_TUH_CDC_PL2303          0
#define CFG_TUH_HID                 4
#define CFG_TUH_MSC                 1
#define CFG_TUH_VENDOR              0
#define CFG_TUH_XINPUT              4
#define CFG_TUH_DEVICE_MAX          4

#define CFG_TUH_HID_EPIN_BUFSIZE    64
#define CFG_TUH_HID_EPOUT_BUFSIZE   64

#define CFG_TUH_CDC_LINE_CONTROL_ON_ENUM  (CDC_CONTROL_LINE_STATE_DTR | CDC_CONTROL_LINE_STATE_RTS)
#define CFG_TUH_CDC_LINE_CODING_ON_ENUM   { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 }

#endif /* _PICO_STDIO_USB_TUSB_CONFIG_H */
