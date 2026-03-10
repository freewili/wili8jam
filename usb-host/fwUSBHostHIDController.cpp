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

    // Capture VID/PID for vendor-specific report parsing
    tuh_vid_pid_get(dev_addr, &inst.vid, &inst.pid);
    printf("[Controller] Mounted: VID=%04X PID=%04X\n", inst.vid, inst.pid);

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

    if (m_reportCallback) {
        m_reportCallback(dev_addr, instance, report, len);
    }

    if (isSonyController(slot))
        processSonyReport(slot, report, len);
    else
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

bool fwUSBHostHIDController::isSonyController(int slot) const {
    if (slot < 0 || slot >= CFG_TUH_HID) return false;
    return m_instances[slot].vid == SONY_VID;
}

void fwUSBHostHIDController::processSonyReport(int slot, uint8_t const *report, uint16_t len) {
    ControllerInstance& inst = m_instances[slot];

    // Detect whether report ID byte is present or stripped
    const uint8_t *data = report;
    uint16_t data_len = len;
    if (data_len > 0 && data[0] == 0x01) {
        data++;
        data_len--;
    }

    // DualSense (PS5) and DualShock 4 (PS4) share stick bytes 0-3 but
    // differ in where buttons live:
    //
    // DualSense:  [LX LY RX RY] [L2trg R2trg counter] [hat+face] [shoulder]
    //              0  1  2  3     4     5     6          7          8
    //
    // DualShock4: [LX LY RX RY] [hat+face] [shoulder] [PS+TP] [L2trg R2trg]
    //              0  1  2  3    4           5          6        7     8

    bool is_dualsense = (inst.pid == SONY_PID_DUALSENSE ||
                         inst.pid == SONY_PID_DUALSENSE_E);
    uint8_t btn_offset = is_dualsense ? 7 : 4;   // hat+face byte
    uint8_t shldr_offset = is_dualsense ? 8 : 5;  // shoulder byte

    if (data_len < (uint16_t)(shldr_offset + 1)) return;

    ControllerState state = {};

    // Axes: LX, LY, RX, RY (uint8, centered at 128)
    state.axes[0] = (int16_t)data[0] - 128; // LX
    state.axes[1] = (int16_t)data[1] - 128; // LY
    state.axes[2] = (int16_t)data[2] - 128; // RX
    state.axes[3] = (int16_t)data[3] - 128; // RY

    // D-pad from low nibble (0=N, 1=NE, 2=E, ... 7=NW, 8=neutral)
    uint8_t dpad = data[btn_offset] & 0x0F;
    uint8_t face = data[btn_offset] >> 4; // Square(0) Cross(1) Circle(2) Triangle(3)

    // Map d-pad value to button bitmask: bits 0-3 = up,down,left,right
    static const uint8_t dpad_map[9] = {
        0x01,       // 0: N     → up
        0x01|0x08,  // 1: NE    → up+right
        0x08,       // 2: E     → right
        0x02|0x08,  // 3: SE    → down+right
        0x02,       // 4: S     → down
        0x02|0x04,  // 5: SW    → down+left
        0x04,       // 6: W     → left
        0x01|0x04,  // 7: NW    → up+left
        0x00,       // 8: neutral
    };
    uint8_t dpad_bits = (dpad <= 8) ? dpad_map[dpad] : 0;

    // Build button bitmask:
    //   bit 0: Cross (South/A equivalent)
    //   bit 1: Circle (East/B equivalent)
    //   bit 2: Square (West)
    //   bit 3: Triangle (North)
    //   bit 4-7: d-pad up,down,left,right
    //   bit 8-15: L1,R1,L2,R2,Create,Options,L3,R3
    state.buttons = 0;
    if (face & 0x02) state.buttons |= (1u << 0);  // Cross
    if (face & 0x04) state.buttons |= (1u << 1);  // Circle
    if (face & 0x01) state.buttons |= (1u << 2);  // Square
    if (face & 0x08) state.buttons |= (1u << 3);  // Triangle
    state.buttons |= ((uint32_t)dpad_bits << 4);   // D-pad in bits 4-7
    state.buttons |= ((uint32_t)data[shldr_offset] << 8); // L1/R1/L2/R2/Create/Options/L3/R3

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
