// Created by bkidwell 1/20/26
// USB HID Generic handler for fwUSBHost
// Handles HID devices that don't use boot protocol

#ifndef FW_USB_HOST_HID_GENERIC_H_
#define FW_USB_HOST_HID_GENERIC_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

#define MAX_GENERIC_REPORTS 4

typedef void (*GenericReportCallback)(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

struct GenericHIDInstance {
    bool      bMounted = false;
    uint8_t   dev_addr = 0;
    uint8_t   instance = 0;
    uint8_t   report_count = 0;
    tuh_hid_report_info_t report_info[MAX_GENERIC_REPORTS];
};

class fwUSBHostHIDGeneric {
private:
    GenericHIDInstance m_instances[CFG_TUH_HID];
    GenericReportCallback m_reportCallback = nullptr;

    int findInstance(uint8_t dev_addr, uint8_t instance);

public:
    fwUSBHostHIDGeneric();

    void mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);
    void task();

    void setReportCallback(GenericReportCallback cb) { m_reportCallback = cb; }

    bool isAnyMounted() const;
    uint8_t getMountedCount() const;
    const tuh_hid_report_info_t* getReportInfo(uint8_t dev_addr, uint8_t instance, uint8_t* count);
};

#endif // FW_USB_HOST_HID_GENERIC_H_
