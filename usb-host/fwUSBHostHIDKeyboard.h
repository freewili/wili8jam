// Created by bkidwell 1/20/26
// USB HID Keyboard handler for fwUSBHost

#ifndef FW_USB_HOST_HID_KEYBOARD_H_
#define FW_USB_HOST_HID_KEYBOARD_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

// Callback type for key events
// keycode: HID keycode, ascii: ASCII character (0 if not printable), pressed: true=pressed, false=released
typedef void (*KeyEventCallback)(uint8_t keycode, char ascii, bool pressed, uint8_t modifiers);

struct KeyboardInstance {
    bool      bMounted = false;
    uint8_t   dev_addr = 0;
    uint8_t   instance = 0;
    hid_keyboard_report_t prev_report = {0, 0, {0}};
};

class fwUSBHostHIDKeyboard {
private:
    KeyboardInstance m_sInstances[CFG_TUH_HID];
    
    KeyEventCallback m_keyCallback = nullptr;
    uint8_t m_currentModifiers = 0;

    int findInstance(uint8_t dev_addr, uint8_t instance);
    static bool findKeyInReport(hid_keyboard_report_t const *report, uint8_t keycode);
    void processReport(int slot, hid_keyboard_report_t const *report);

public:
    fwUSBHostHIDKeyboard();

    // TinyUSB callback handlers (called by fwUSBHostHID)
    void mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);
    void task();

    // Set callback for key events (optional)
    void setKeyCallback(KeyEventCallback cb) { m_keyCallback = cb; }

    // Query state
    bool isAnyMounted() const;
    uint8_t getMountedCount() const;
    
    // Event interface
    bool hasKeys() const;           // Returns true if keys are buffered
    char getLastKey();              // Pop next key from buffer (0 if empty)
    uint8_t getModifiers() const { return m_currentModifiers; }

    // Utility
    static char keycodeToAscii(uint8_t keycode, bool shift);
};

#endif // FW_USB_HOST_HID_KEYBOARD_H_
