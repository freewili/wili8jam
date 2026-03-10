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
    tusb_desc_device_t desc;
    if (tuh_descriptor_get_device_sync(dev_addr, &desc, sizeof(desc)) == XFER_RESULT_SUCCESS) {
        printf("[USB] dev %d: VID=%04x PID=%04x class=%d subclass=%d protocol=%d\n",
            dev_addr, desc.idVendor, desc.idProduct,
            desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol);
    } else {
        printf("[USB] dev %d: mounted (descriptor read failed)\n", dev_addr);
    }
}

void tuh_umount_cb(uint8_t dev_addr) {
    (void)dev_addr;
}

} // extern "C"
