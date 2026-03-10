// Created by bkidwell 3/7/26
// USB HID Controller (Gamepad/Joystick) handler for fwUSBHost

#include "fwUSBHostHIDController.h"
#include <string.h>

fwUSBHostHIDController::fwUSBHostHIDController() {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        m_instances[i] = {};
    }
}

int fwUSBHostHIDController::findInstance(uint8_t dev_addr, uint8_t instance) const {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted &&
            m_instances[i].dev_addr == dev_addr &&
            m_instances[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

bool fwUSBHostHIDController::isController(uint8_t const *desc_report, uint16_t desc_len) {
    tuh_hid_report_info_t info[4];
    uint8_t count = tuh_hid_parse_report_descriptor(info, 4, desc_report, desc_len);

    for (uint8_t i = 0; i < count; i++) {
        if (info[i].usage_page == HID_USAGE_PAGE_DESKTOP &&
            (info[i].usage == HID_USAGE_DESKTOP_GAMEPAD ||
             info[i].usage == HID_USAGE_DESKTOP_JOYSTICK)) {
            return true;
        }
    }
    return false;
}

void fwUSBHostHIDController::mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    int slot = -1;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (!m_instances[i].bMounted) {
            slot = i;
            break;
        }
    }

    if (slot == -1) return;

    ControllerInstance& inst = m_instances[slot];
    inst.bMounted = true;
    inst.dev_addr = dev_addr;
    inst.instance = instance;
    inst.prev_state = {};

    printf("[Controller] Mounted slot=%d dev=%d inst=%d\n", slot, dev_addr, instance);

    inst.report_count = tuh_hid_parse_report_descriptor(
        inst.report_info,
        4,
        desc_report,
        desc_len
    );
}

void fwUSBHostHIDController::unmount(uint8_t dev_addr, uint8_t instance) {
    int slot = findInstance(dev_addr, instance);
    if (slot >= 0) {
        m_instances[slot] = {};
    }
}

void fwUSBHostHIDController::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return;

    // Debug: print first few bytes of report
    printf("[Controller] report addr=%d inst=%d len=%d:", dev_addr, instance, len);
    for (int i = 0; i < len && i < 8; i++) printf(" %02x", report[i]);
    printf("\n");

    if (m_reportCallback) {
        m_reportCallback(dev_addr, instance, report, len);
    }

    processReport(slot, report, len);
}

void fwUSBHostHIDController::processReport(int slot, uint8_t const *report, uint16_t len) {
    ControllerInstance& inst = m_instances[slot];

    // Generic gamepad report parsing
    // Most controllers send: [buttons_lo, buttons_hi, x, y, z, rz, ...]
    // This handles common layouts; specific controllers may need adaptation
    ControllerState state = {};

    if (len >= 2) {
        // First two bytes are typically button bitmask
        state.buttons = report[0] | ((uint32_t)report[1] << 8);
    }
    if (len >= 4) {
        // Bytes after buttons are typically axes (uint8 centered at 128)
        uint16_t axis_offset = 2;
        for (uint8_t a = 0; a < CONTROLLER_MAX_AXES && (axis_offset + a) < len; a++) {
            // Convert unsigned 0-255 to signed -128..127
            state.axes[a] = (int16_t)report[axis_offset + a] - 128;
        }
    }

    // Fire button change callbacks
    if (m_buttonCallback) {
        uint32_t changed = state.buttons ^ inst.prev_state.buttons;
        for (uint8_t b = 0; b < CONTROLLER_MAX_BUTTONS; b++) {
            if (changed & (1u << b)) {
                m_buttonCallback(b, (state.buttons >> b) & 1);
            }
        }
    }

    // Fire axis change callbacks
    if (m_axisCallback) {
        for (uint8_t a = 0; a < CONTROLLER_MAX_AXES; a++) {
            if (state.axes[a] != inst.prev_state.axes[a]) {
                m_axisCallback(a, state.axes[a]);
            }
        }
    }

    m_currentState = state;
    inst.prev_state = state;
}

void fwUSBHostHIDController::task() {
    // Nothing to do - event driven
}

bool fwUSBHostHIDController::isAnyMounted() const {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) return true;
    }
    return false;
}

uint8_t fwUSBHostHIDController::getMountedCount() const {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) count++;
    }
    return count;
}

int16_t fwUSBHostHIDController::getAxis(uint8_t axis) const {
    if (axis >= CONTROLLER_MAX_AXES) return 0;
    return m_currentState.axes[axis];
}

int fwUSBHostHIDController::getPlayerForDevice(uint8_t dev_addr, uint8_t instance) const {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return 0;

    // Count how many mounted instances come before this slot
    int player = 0;
    for (int i = 0; i < slot; ++i) {
        if (m_instances[i].bMounted) player++;
    }
    return player;
}
