// Created by bkidwell 3/7/26
// USB XInput (Xbox controller) handler for fwUSBHost

#ifndef FW_USB_HOST_XINPUT_H_
#define FW_USB_HOST_XINPUT_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"
#include "xinput_host.h"

struct XInputInstance {
    bool      bMounted = false;
    bool      bConnected = false;
    uint8_t   dev_addr = 0;
    uint8_t   instance = 0;
    xinput_type_t type = XINPUT_UNKNOWN;
};

// Callback for normalized XInput pad data
typedef void (*XInputReportCallback)(uint8_t dev_addr, uint8_t instance, xinput_gamepad_t const *pad);

class fwUSBHostXInput {
private:
    XInputInstance m_instances[CFG_TUH_XINPUT];
    XInputReportCallback m_reportCallback = nullptr;

    int findInstance(uint8_t dev_addr, uint8_t instance) const;

public:
    fwUSBHostXInput();

    // TinyUSB xinput callback handlers
    void mount(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xid_itf);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const *xid_itf);
    void task();

    // Set callback (optional)
    void setReportCallback(XInputReportCallback cb) { m_reportCallback = cb; }

    // Query state
    bool isAnyMounted() const;
    uint8_t getMountedCount() const;

    // Get the player index (0-based) for a mounted controller instance
    int getPlayerForDevice(uint8_t dev_addr, uint8_t instance) const;
};

#endif // FW_USB_HOST_XINPUT_H_
