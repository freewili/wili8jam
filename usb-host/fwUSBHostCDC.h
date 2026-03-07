// Created by bkidwell 1/20/26
// USB CDC Application Task for fwUSBHost

#ifndef FW_USB_HOST_CDC_H_
#define FW_USB_HOST_CDC_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

struct CDCInstance {
    tuh_itf_info_t itf_info = {};
    bool      bMounted = false;
    uint8_t   btDevAddr = 0;
    uint8_t   btInterfaceNumber = 0;
    uint32_t  iBaudrate = 0;
    uint8_t   bStopBits = 0;
    uint8_t   bParity = 0;
    uint8_t   bDataWidth = 0;
};

class fwUSBHostCDC {
private:
    CDCInstance m_sCDCInstance[CFG_TUH_CDC];

public:
    fwUSBHostCDC();

    void task();

    // TinyUSB callback handlers
    void rx(uint8_t idx);
    void mount(uint8_t idx);
    void unmount(uint8_t idx);

    static size_t get_console_inputs(uint8_t* buf, size_t bufsize);

    // Per-device getters
    bool isMounted(uint8_t idx);
    uint8_t getDevAddr(uint8_t idx);
    uint8_t getInterfaceNumber(uint8_t idx);
    uint32_t getBaudrate(uint8_t idx);
    uint8_t getStopBits(uint8_t idx);
    uint8_t getParity(uint8_t idx);
    uint8_t getDataWidth(uint8_t idx);
    
    uint8_t getFirstMountedIdx();
    uint8_t getMountedCount();
};

#endif // FW_USB_HOST_CDC_H_
