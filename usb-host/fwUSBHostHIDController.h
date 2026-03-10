// Created by bkidwell 3/7/26
// USB HID Controller (Gamepad/Joystick) handler for fwUSBHost

#ifndef FW_USB_HOST_HID_CONTROLLER_H_
#define FW_USB_HOST_HID_CONTROLLER_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

// HID Usage Page / Usage IDs for controller detection
#define HID_USAGE_PAGE_DESKTOP    0x01
#define HID_USAGE_DESKTOP_JOYSTICK  0x04
#define HID_USAGE_DESKTOP_GAMEPAD   0x05

// Maximum number of axes and buttons we track per controller
#define CONTROLLER_MAX_AXES    6
#define CONTROLLER_MAX_BUTTONS 16

// Callback type for raw controller reports (matches GenericReportCallback)
typedef void (*ControllerReportCallback)(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

// Callback type for controller events
typedef void (*ControllerButtonCallback)(uint8_t button, bool pressed);
typedef void (*ControllerAxisCallback)(uint8_t axis, int16_t value);

struct ControllerState {
    int16_t  axes[CONTROLLER_MAX_AXES];
    uint32_t buttons;  // Bitmask of up to CONTROLLER_MAX_BUTTONS
};

struct ControllerInstance {
    bool      bMounted = false;
    uint8_t   dev_addr = 0;
    uint8_t   instance = 0;
    uint8_t   report_count = 0;
    tuh_hid_report_info_t report_info[4];
    ControllerState prev_state = {};
};

class fwUSBHostHIDController {
private:
    ControllerInstance m_instances[CFG_TUH_HID];

    ControllerReportCallback m_reportCallback = nullptr;
    ControllerButtonCallback m_buttonCallback = nullptr;
    ControllerAxisCallback m_axisCallback = nullptr;

    // Current state (merged from all controllers)
    ControllerState m_currentState = {};

    int findInstance(uint8_t dev_addr, uint8_t instance) const;
    void processReport(int slot, uint8_t const *report, uint16_t len);

public:
    fwUSBHostHIDController();

    // Check if a HID report descriptor describes a controller
    static bool isController(uint8_t const *desc_report, uint16_t desc_len);

    // TinyUSB callback handlers (called by fwUSBHostHID)
    void mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len);
    void unmount(uint8_t dev_addr, uint8_t instance);
    void report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);
    void task();

    // Set callbacks (optional)
    void setReportCallback(ControllerReportCallback cb) { m_reportCallback = cb; }
    void setButtonCallback(ControllerButtonCallback cb) { m_buttonCallback = cb; }
    void setAxisCallback(ControllerAxisCallback cb) { m_axisCallback = cb; }

    // Get the player index (0-based) for a mounted controller instance
    int getPlayerForDevice(uint8_t dev_addr, uint8_t instance) const;

    // Query state
    bool isAnyMounted() const;
    uint8_t getMountedCount() const;

    // Button state
    uint32_t getButtons() const { return m_currentState.buttons; }
    bool isButtonPressed(uint8_t button) const { return (m_currentState.buttons >> button) & 1; }

    // Axis state
    int16_t getAxis(uint8_t axis) const;
};

#endif // FW_USB_HOST_HID_CONTROLLER_H_
