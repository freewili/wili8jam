// Created by bkidwell 1/20/26
// USB HID Keyboard handler for fwUSBHost

#include "fwUSBHostHIDKeyboard.h"
#include <string.h>

// HID Keycode to ASCII conversion table [keycode][shift]
static const uint8_t s_keycode2ascii[128][2] = { HID_KEYCODE_TO_ASCII };

// Simple ring buffer for key presses
static const int KEY_BUFFER_SIZE = 16;
static char s_keyBuffer[KEY_BUFFER_SIZE];
static volatile int s_keyBufferHead = 0;
static volatile int s_keyBufferTail = 0;

static void pushKey(char ch) {
    int next = (s_keyBufferHead + 1) % KEY_BUFFER_SIZE;
    if (next != s_keyBufferTail) {
        s_keyBuffer[s_keyBufferHead] = ch;
        s_keyBufferHead = next;
    }
}

static char popKey() {
    if (s_keyBufferHead == s_keyBufferTail) {
        return 0;
    }
    char ch = s_keyBuffer[s_keyBufferTail];
    s_keyBufferTail = (s_keyBufferTail + 1) % KEY_BUFFER_SIZE;
    return ch;
}

static bool hasBufferedKeys() {
    return s_keyBufferHead != s_keyBufferTail;
}

fwUSBHostHIDKeyboard::fwUSBHostHIDKeyboard() {
    memset(m_sInstances, 0, sizeof(m_sInstances));
    m_currentModifiers = 0;
    m_keyCallback = nullptr;
}

int fwUSBHostHIDKeyboard::findInstance(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_sInstances[i].bMounted && 
            m_sInstances[i].dev_addr == dev_addr && 
            m_sInstances[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

bool fwUSBHostHIDKeyboard::findKeyInReport(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (uint8_t i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    return false;
}

char fwUSBHostHIDKeyboard::keycodeToAscii(uint8_t keycode, bool shift) {
    if (keycode >= 128) return 0;
    return (char)s_keycode2ascii[keycode][shift ? 1 : 0];
}

void fwUSBHostHIDKeyboard::mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    (void)desc_report;
    (void)desc_len;

    int slot = -1;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (!this->m_sInstances[i].bMounted) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) return;

    this->m_sInstances[slot].bMounted = true;
    this->m_sInstances[slot].dev_addr = dev_addr;
    this->m_sInstances[slot].instance = instance;
    this->m_sInstances[slot].prev_report = {0, 0, {0}};
}

void fwUSBHostHIDKeyboard::unmount(uint8_t dev_addr, uint8_t instance) {
    int slot = findInstance(dev_addr, instance);
    if (slot >= 0) {
        m_sInstances[slot] = {};
    }
}

void fwUSBHostHIDKeyboard::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)len;

    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return;

    hid_keyboard_report_t const *kbd_report = (hid_keyboard_report_t const *)report;
    processReport(slot, kbd_report);
}

void fwUSBHostHIDKeyboard::processReport(int slot, hid_keyboard_report_t const *report) {
    KeyboardInstance& inst = m_sInstances[slot];
    
    // Update current modifier state
    m_currentModifiers = report->modifier;
    
    bool const is_shift = report->modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT);

    // Check for newly pressed keys
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode == 0) continue;

        if (!findKeyInReport(&inst.prev_report, keycode)) {
            char ascii = keycodeToAscii(keycode, is_shift);
            
            if (ascii != 0) {
                pushKey(ascii);
            }

            if (m_keyCallback) {
                m_keyCallback(keycode, ascii, true, report->modifier);
            }
        }
    }

    // Check for released keys
    for (uint8_t i = 0; i < 6; i++) {
        uint8_t keycode = inst.prev_report.keycode[i];
        if (keycode == 0) continue;

        if (!findKeyInReport(report, keycode)) {
            char ascii = keycodeToAscii(keycode, is_shift);
            
            if (m_keyCallback) {
                m_keyCallback(keycode, ascii, false, report->modifier);
            }
        }
    }

    inst.prev_report = *report;
}

void fwUSBHostHIDKeyboard::task() {
    // Nothing to do - event driven
}

bool fwUSBHostHIDKeyboard::isAnyMounted() const {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_sInstances[i].bMounted) return true;
    }
    return false;
}

uint8_t fwUSBHostHIDKeyboard::getMountedCount() const {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_sInstances[i].bMounted) count++;
    }
    return count;
}

bool fwUSBHostHIDKeyboard::hasKeys() const {
    return hasBufferedKeys();
}

char fwUSBHostHIDKeyboard::getLastKey() {
    return popKey();
}
