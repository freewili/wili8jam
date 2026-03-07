// Created 1/18/26 by bkidwell
// https://docs.tinyusb.org/en/latest/integration.html

#ifndef FW_USB_HOST_H_
#define FW_USB_HOST_H_

#include <stdint.h>
#include "pico/stdlib.h"
#include "fwUSBHostMSC.h"
#include "fwUSBHostCDC.h"
#include "fwUSBHostHID.h"

class fwUSBHost {
public:
    fwUSBHost();
    
    // Initialize USB host stack
    void init();

    // Call regularly to poll USB stack
    void task();
    
    // Check if there are events to process
    bool hasEvents() { return m_obHID.hasEvents(); }

    // Child class handlers (public for TUSB callback access)
    fwUSBHostMSC m_obMSC;
    fwUSBHostCDC m_obCDC;
    fwUSBHostHID m_obHID;

    // Convenience accessors
    bool isKeyboardConnected() { return m_obHID.isKeyboardMounted(); }
    bool isMouseConnected()    { return m_obHID.isMouseMounted(); }
    bool isMSCConnected()      { return m_obMSC.getMountedCount() > 0; }
    bool isCDCConnected()      { return m_obCDC.getMountedCount() > 0; }
};

#endif // FW_USB_HOST_H_
