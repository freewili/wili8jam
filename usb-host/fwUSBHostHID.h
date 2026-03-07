// Created by bkidwell 1/20/26
// USB HID Application Task for fwUSBHost

#ifndef FW_USB_HOST_HID_H_
#define FW_USB_HOST_HID_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

#include "fwUSBHostHIDKeyboard.h"
#include "fwUSBHostHIDMouse.h"
#include "fwUSBHostHIDGeneric.h"

class fwUSBHostHID {
private:
    fwUSBHostHIDKeyboard m_obKeyboard;
    fwUSBHostHIDMouse m_obMouse;
    fwUSBHostHIDGeneric m_obGeneric;

    uint8_t m_instanceProtocol[CFG_TUH_HID];

public:
    fwUSBHostHID();

    void task();

    // TinyUSB callback handlers
    void mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

    // Event interface
    bool hasEvents() const { 
        return m_obKeyboard.hasKeys() || m_obMouse.hasMovement(); 
    }

    // State queries
    bool isKeyboardMounted() const { return m_obKeyboard.isAnyMounted(); }
    bool isMouseMounted() const    { return m_obMouse.isAnyMounted(); }
    bool isGenericMounted() const  { return m_obGeneric.isAnyMounted(); }
    
    uint8_t getKeyboardCount() const { return m_obKeyboard.getMountedCount(); }
    uint8_t getMouseCount() const    { return m_obMouse.getMountedCount(); }
    uint8_t getGenericCount() const  { return m_obGeneric.getMountedCount(); }

    // Direct child access
    fwUSBHostHIDKeyboard& getKeyboard() { return m_obKeyboard; }
    fwUSBHostHIDMouse& getMouse()       { return m_obMouse; }
    fwUSBHostHIDGeneric& getGeneric()   { return m_obGeneric; }
    
    const fwUSBHostHIDKeyboard& getKeyboard() const { return m_obKeyboard; }
    const fwUSBHostHIDMouse& getMouse() const       { return m_obMouse; }
    const fwUSBHostHIDGeneric& getGeneric() const   { return m_obGeneric; }
};

#endif // FW_USB_HOST_HID_H_
