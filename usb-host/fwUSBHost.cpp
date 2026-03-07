// Created 1/18/26 by bkidwell
// https://docs.tinyusb.org/en/latest/integration.html

#include <stdlib.h>
#include <stdio.h>

#include "tusb_config.h"
#include "fwUSBHost.h"
#include "pico/stdlib.h"

extern "C" {
#include "tusb.h"
}

fwUSBHost obUSBHost;

fwUSBHost::fwUSBHost() {
}

void fwUSBHost::init() {
    // Initialize TinyUSB Host Stack natively
    tuh_init(BOARD_TUH_RHPORT);
}

void fwUSBHost::task() {
    // Poll TinyUSB
    tuh_task();

    // Run child tasks
    m_obCDC.task();
    m_obHID.task();
}

//------------- TinyUSB Callbacks -------------//

extern "C" {

void tuh_mount_cb(uint8_t dev_addr) {
    (void)dev_addr;
}

void tuh_umount_cb(uint8_t dev_addr) {
    (void)dev_addr;
}

} // extern "C"
