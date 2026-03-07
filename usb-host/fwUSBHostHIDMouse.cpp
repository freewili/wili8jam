// Created by bkidwell 1/20/26
// USB HID Mouse handler for fwUSBHost

#include "fwUSBHostHIDMouse.h"
#include <string.h>

fwUSBHostHIDMouse::fwUSBHostHIDMouse() {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        m_instances[i] = {};
    }
}

int fwUSBHostHIDMouse::findInstance(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted && 
            m_instances[i].dev_addr == dev_addr && 
            m_instances[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

void fwUSBHostHIDMouse::mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    int slot = -1;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (!m_instances[i].bMounted) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) return;

    m_instances[slot].bMounted = true;
    m_instances[slot].dev_addr = dev_addr;
    m_instances[slot].instance = instance;
    m_instances[slot].prev_report = {0};
}

void fwUSBHostHIDMouse::unmount(uint8_t dev_addr, uint8_t instance) {
    int slot = findInstance(dev_addr, instance);
    if (slot >= 0) {
        m_instances[slot] = {};
    }
}

void fwUSBHostHIDMouse::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)len;

    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return;

    hid_mouse_report_t const *mouse_report = (hid_mouse_report_t const *)report;
    processReport(slot, mouse_report);
}

void fwUSBHostHIDMouse::processReport(int slot, hid_mouse_report_t const *report) {
    MouseInstance& inst = m_instances[slot];

    // Accumulate movement
    m_accumX += report->x;
    m_accumY += report->y;
    m_accumWheel += report->wheel;
    
    // Track button state for hasMovement() detection
    m_prevButtons = m_currentButtons;
    m_currentButtons = report->buttons;

    // Check for button changes and fire callbacks
    uint8_t button_changed = report->buttons ^ inst.prev_report.buttons;
    
    if (button_changed && m_buttonCallback) {
        if (button_changed & MOUSE_BUTTON_LEFT) {
            m_buttonCallback(MOUSE_BUTTON_LEFT, report->buttons & MOUSE_BUTTON_LEFT);
        }
        if (button_changed & MOUSE_BUTTON_RIGHT) {
            m_buttonCallback(MOUSE_BUTTON_RIGHT, report->buttons & MOUSE_BUTTON_RIGHT);
        }
        if (button_changed & MOUSE_BUTTON_MIDDLE) {
            m_buttonCallback(MOUSE_BUTTON_MIDDLE, report->buttons & MOUSE_BUTTON_MIDDLE);
        }
        if (button_changed & MOUSE_BUTTON_BACKWARD) {
            m_buttonCallback(MOUSE_BUTTON_BACKWARD, report->buttons & MOUSE_BUTTON_BACKWARD);
        }
        if (button_changed & MOUSE_BUTTON_FORWARD) {
            m_buttonCallback(MOUSE_BUTTON_FORWARD, report->buttons & MOUSE_BUTTON_FORWARD);
        }
    }

    if (m_moveCallback) {
        m_moveCallback(report->buttons, report->x, report->y, report->wheel);
    }

    inst.prev_report = *report;
}

void fwUSBHostHIDMouse::task() {
    // Nothing to do - event driven
}

bool fwUSBHostHIDMouse::isAnyMounted() const {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) return true;
    }
    return false;
}

uint8_t fwUSBHostHIDMouse::getMountedCount() const {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) count++;
    }
    return count;
}

bool fwUSBHostHIDMouse::hasMovement() const {
    return (m_accumX != 0) || (m_accumY != 0) || (m_accumWheel != 0);
}

int32_t fwUSBHostHIDMouse::getDeltaX() {
    int32_t val = m_accumX;
    m_accumX = 0;
    return val;
}

int32_t fwUSBHostHIDMouse::getDeltaY() {
    int32_t val = m_accumY;
    m_accumY = 0;
    return val;
}

int32_t fwUSBHostHIDMouse::getDeltaWheel() {
    int32_t val = m_accumWheel;
    m_accumWheel = 0;
    return val;
}

void fwUSBHostHIDMouse::getDeltas(int32_t* x, int32_t* y, int32_t* wheel) {
    if (x) *x = m_accumX;
    if (y) *y = m_accumY;
    if (wheel) *wheel = m_accumWheel;
    m_accumX = 0;
    m_accumY = 0;
    m_accumWheel = 0;
}
