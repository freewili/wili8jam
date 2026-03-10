// Created by bkidwell 1/20/26
// USB HID Application Task for fwUSBHost

#include "fwUSBHostHID.h"
#include "fwUSBHost.h"
#include <string.h>

extern fwUSBHost obUSBHost;

fwUSBHostHID::fwUSBHostHID() {
    for (int i = 0; i < CFG_TUH_HID; ++i) {
        m_instanceProtocol[i] = HID_ITF_PROTOCOL_NONE;
    }
}

void fwUSBHostHID::task() {
    m_obKeyboard.task();
    m_obMouse.task();
    m_obController.task();
    m_obGeneric.task();
}

void fwUSBHostHID::mount(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    m_instanceProtocol[instance] = itf_protocol;

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            m_obKeyboard.mount(dev_addr, instance, desc_report, desc_len);
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            m_obMouse.mount(dev_addr, instance, desc_report, desc_len);
            break;

        case HID_ITF_PROTOCOL_NONE:
        default:
            if (fwUSBHostHIDController::isController(desc_report, desc_len)) {
                printf("[HID] addr=%d inst=%d → Controller\n", dev_addr, instance);
                m_obController.mount(dev_addr, instance, desc_report, desc_len);
            } else {
                printf("[HID] addr=%d inst=%d → Generic\n", dev_addr, instance);
                m_obGeneric.mount(dev_addr, instance, desc_report, desc_len);
            }
            break;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void fwUSBHostHID::unmount(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = m_instanceProtocol[instance];

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            m_obKeyboard.unmount(dev_addr, instance);
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            m_obMouse.unmount(dev_addr, instance);
            break;

        case HID_ITF_PROTOCOL_NONE:
        default:
            m_obController.unmount(dev_addr, instance);
            m_obGeneric.unmount(dev_addr, instance);
            break;
    }

    m_instanceProtocol[instance] = HID_ITF_PROTOCOL_NONE;
}

void fwUSBHostHID::report_received(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = m_instanceProtocol[instance];

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            m_obKeyboard.report_received(dev_addr, instance, report, len);
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            m_obMouse.report_received(dev_addr, instance, report, len);
            break;

        case HID_ITF_PROTOCOL_NONE:
        default:
            m_obController.report_received(dev_addr, instance, report, len);
            m_obGeneric.report_received(dev_addr, instance, report, len);
            break;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

//------------- TinyUSB Callbacks -------------//

extern "C" {

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    obUSBHost.m_obHID.mount(dev_addr, instance, desc_report, desc_len);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    obUSBHost.m_obHID.unmount(dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    obUSBHost.m_obHID.report_received(dev_addr, instance, report, len);
}

} // extern "C"
