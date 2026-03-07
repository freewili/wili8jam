// Created by bkidwell 1/20/26
// USB HID Mouse handler for fwUSBHost

#ifndef FW_USB_HOST_HID_MOUSE_H_
#define FW_USB_HOST_HID_MOUSE_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

// Callback type for mouse events
typedef void (*MouseEventCallback)(uint8_t buttons, int8_t x, int8_t y, int8_t wheel);
typedef void (*MouseButtonCallback)(uint8_t button, bool pressed);

struct MouseInstance {
    bool      bMounted = false;
    uint8_t   dev_addr = 0;
    uint8_t   instance = 0;
    hid_mouse_report_t prev_report = {0};
};

class fwUSBHostHIDMouse {
private:
    MouseInstance m_instances[CFG_TUH_HID];
    
    MouseEventCallback m_moveCallback = nullptr;
    MouseButtonCallback m_buttonCallback = nullptr;
    
    // Accumulated movement since last read
    volatile int32_t m_accumX = 0;
    volatile int32_t m_accumY = 0;
    volatile int32_t m_accumWheel = 0;
    volatile uint8_t m_currentButtons = 0;
    volatile uint8_t m_prevButtons = 0;  // For detecting button events

    int findInstance(uint8_t dev_addr, uint8_t instance);
    void processReport(int slot, hid_mouse_report_t const *report);

public:
    fwUSBHostHIDMouse();

    // TinyUSB callback handlers
    void mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);
    void task();

    // Set callbacks (optional)
    void setMoveCallback(MouseEventCallback cb) { m_moveCallback = cb; }
    void setButtonCallback(MouseButtonCallback cb) { m_buttonCallback = cb; }

    // Query state
    bool isAnyMounted() const;
    uint8_t getMountedCount() const;

    // Event interface
    bool hasMovement() const;   // True if any accumulated movement or button change
    
    // Polling interface - resets accumulators after reading
    int32_t getDeltaX();
    int32_t getDeltaY();
    int32_t getDeltaWheel();
    void getDeltas(int32_t* x, int32_t* y, int32_t* wheel);
    
    // Button state (doesn't reset)
    uint8_t getButtons() const { return m_currentButtons; }
    bool isLeftPressed() const { return m_currentButtons & MOUSE_BUTTON_LEFT; }
    bool isRightPressed() const { return m_currentButtons & MOUSE_BUTTON_RIGHT; }
    bool isMiddlePressed() const { return m_currentButtons & MOUSE_BUTTON_MIDDLE; }
};

#endif // FW_USB_HOST_HID_MOUSE_H_
