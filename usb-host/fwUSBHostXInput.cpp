// Created by bkidwell 3/7/26
// USB XInput (Xbox controller) handler for fwUSBHost

#include "fwUSBHostXInput.h"
#include "fwUSBHost.h"
#include <string.h>

extern fwUSBHost obUSBHost;

fwUSBHostXInput::fwUSBHostXInput() {
    for (int i = 0; i < CFG_TUH_XINPUT; ++i) {
        m_instances[i] = {};
    }
}

int fwUSBHostXInput::findInstance(uint8_t dev_addr, uint8_t instance) const {
    for (int i = 0; i < CFG_TUH_XINPUT; ++i) {
        if (m_instances[i].bMounted &&
            m_instances[i].dev_addr == dev_addr &&
            m_instances[i].instance == instance) {
            return i;
        }
    }
    return -1;
}

void fwUSBHostXInput::mount(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xid_itf) {
    int slot = -1;
    for (int i = 0; i < CFG_TUH_XINPUT; ++i) {
        if (!m_instances[i].bMounted) {
            slot = i;
            break;
        }
    }

    if (slot == -1) return;

    m_instances[slot].bMounted = true;
    m_instances[slot].bConnected = xid_itf->connected;
    m_instances[slot].dev_addr = dev_addr;
    m_instances[slot].instance = instance;
    m_instances[slot].type = xid_itf->type;

    const char *type_str = "unknown";
    switch (xid_itf->type) {
        case XBOXONE:          type_str = "Xbox One/Series"; break;
        case XBOX360_WIRED:    type_str = "Xbox 360 Wired"; break;
        case XBOX360_WIRELESS: type_str = "Xbox 360 Wireless"; break;
        case XBOXOG:           type_str = "Xbox OG"; break;
        default: break;
    }
    printf("[XInput] Mounted: %s (addr=%d inst=%d)\n", type_str, dev_addr, instance);

    // Start receiving reports
    tuh_xinput_receive_report(dev_addr, instance);
}

void fwUSBHostXInput::unmount(uint8_t dev_addr, uint8_t instance) {
    int slot = findInstance(dev_addr, instance);
    if (slot >= 0) {
        printf("[XInput] Unmounted (addr=%d inst=%d)\n", dev_addr, instance);
        m_instances[slot] = {};
    }
}

void fwUSBHostXInput::report_received(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const *xid_itf) {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return;

    m_instances[slot].bConnected = xid_itf->connected;

    if (!xid_itf->connected) return;

    if (m_reportCallback) {
        m_reportCallback(dev_addr, instance, &xid_itf->pad);
    }

    // Request next report
    tuh_xinput_receive_report(dev_addr, instance);
}

void fwUSBHostXInput::task() {
    // Nothing to do - event driven
}

bool fwUSBHostXInput::isAnyMounted() const {
    for (int i = 0; i < CFG_TUH_XINPUT; ++i) {
        if (m_instances[i].bMounted && m_instances[i].bConnected) return true;
    }
    return false;
}

uint8_t fwUSBHostXInput::getMountedCount() const {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_XINPUT; ++i) {
        if (m_instances[i].bMounted && m_instances[i].bConnected) count++;
    }
    return count;
}

int fwUSBHostXInput::getPlayerForDevice(uint8_t dev_addr, uint8_t instance) const {
    int slot = findInstance(dev_addr, instance);
    if (slot < 0) return 0;

    int player = 0;
    for (int i = 0; i < slot; ++i) {
        if (m_instances[i].bMounted && m_instances[i].bConnected) player++;
    }
    return player;
}

//------------- TinyUSB XInput Callbacks -------------//

extern "C" {

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xid_itf) {
    obUSBHost.m_obXInput.mount(dev_addr, instance, xid_itf);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    obUSBHost.m_obXInput.unmount(dev_addr, instance);
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance, xinputh_interface_t const *xid_itf, uint16_t len) {
    (void)len;
    obUSBHost.m_obXInput.report_received(dev_addr, instance, xid_itf);
}

} // extern "C"
