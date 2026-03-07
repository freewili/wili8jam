// Created by bkidwell 1/20/26
// USB HID Generic handler for fwUSBHost

#include "fwUSBHostHIDGeneric.h"
#include <string.h>

fwUSBHostHIDGeneric::fwUSBHostHIDGeneric() {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        m_instances[i] = {};
    }
}

int fwUSBHostHIDGeneric::findInstance(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted && 
            m_instances[i].dev_addr == dev_addr && 
            m_instances[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

void fwUSBHostHIDGeneric::mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    int slot = -1;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (!m_instances[i].bMounted) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) return;

    GenericHIDInstance& inst = m_instances[slot];
    inst.bMounted = true;
    inst.dev_addr = dev_addr;
    inst.instance = instance;

    inst.report_count = tuh_hid_parse_report_descriptor(
        inst.report_info, 
        MAX_GENERIC_REPORTS, 
        desc_report, 
        desc_len
    );
}

void fwUSBHostHIDGeneric::unmount(uint8_t dev_addr, uint8_t instance) {
    int slot = findInstance(dev_addr, instance);
    if (slot >= 0) {
        m_instances[slot] = {};
    }
}

void fwUSBHostHIDGeneric::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return;

    if (m_reportCallback) {
        m_reportCallback(dev_addr, instance, report, len);
    }
}

void fwUSBHostHIDGeneric::task() {
    // Nothing to do
}

bool fwUSBHostHIDGeneric::isAnyMounted() const {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) return true;
    }
    return false;
}

uint8_t fwUSBHostHIDGeneric::getMountedCount() const {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        if (m_instances[i].bMounted) count++;
    }
    return count;
}

const tuh_hid_report_info_t* fwUSBHostHIDGeneric::getReportInfo(uint8_t dev_addr, uint8_t instance, uint8_t* count) {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) {
        if (count) *count = 0;
        return nullptr;
    }
    
    if (count) *count = m_instances[slot].report_count;
    return m_instances[slot].report_info;
}
