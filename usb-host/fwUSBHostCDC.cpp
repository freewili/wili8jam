// Created by bkidwell 1/20/26
// USB CDC Application Task for fwUSBHost

#include "fwUSBHost.h"
#include "fwUSBHostCDC.h"

extern fwUSBHost obUSBHost;

fwUSBHostCDC::fwUSBHostCDC() {
}

void fwUSBHostCDC::task() {
    // Removed legacy stdin -> CDC bridging (UART is not available)
}

void fwUSBHostCDC::rx(uint8_t idx) {
    uint8_t buf[64 + 1];
    uint32_t const bufsize = sizeof(buf) - 1;

    const uint32_t count = tuh_cdc_read(idx, buf, bufsize);
    if (count) {
        // Data received handling goes here
        // (Removed printf as UART is unavailable)
    }
}

void fwUSBHostCDC::mount(uint8_t idx) {
    if (idx >= CFG_TUH_CDC) return;
    
    CDCInstance& inst = m_sCDCInstance[idx];
    
    tuh_itf_info_t itf_info = {0};
    tuh_cdc_itf_get_info(idx, &itf_info);

    inst.itf_info = itf_info;
    inst.bMounted = true;
    inst.btDevAddr = itf_info.daddr;
    inst.btInterfaceNumber = itf_info.desc.bInterfaceNumber;

#ifdef CFG_TUH_CDC_LINE_CODING_ON_ENUM
    cdc_line_coding_t line_coding = {0};
    if (tuh_cdc_get_local_line_coding(idx, &line_coding)) {
        inst.iBaudrate = line_coding.bit_rate;
        inst.bStopBits = line_coding.stop_bits;
        inst.bParity = line_coding.parity;
        inst.bDataWidth = line_coding.data_bits;
    }
#else
    cdc_line_coding_t new_line_coding = { 115200, CDC_LINE_CODING_STOP_BITS_1, CDC_LINE_CODING_PARITY_NONE, 8 };
    tuh_cdc_set_line_coding(idx, &new_line_coding, NULL, 0);
    inst.iBaudrate = new_line_coding.bit_rate;
    inst.bStopBits = new_line_coding.stop_bits;
    inst.bParity = new_line_coding.parity;
    inst.bDataWidth = new_line_coding.data_bits;
#endif
}

void fwUSBHostCDC::unmount(uint8_t idx) {
    if (idx >= CFG_TUH_CDC) return;
    m_sCDCInstance[idx] = {};
}

size_t fwUSBHostCDC::get_console_inputs(uint8_t* buf, size_t bufsize) {
    // UART is unavailable on this processor, stubbed out
    return 0;
}

bool fwUSBHostCDC::isMounted(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].bMounted;
    }
    return false;
}

uint8_t fwUSBHostCDC::getDevAddr(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].btDevAddr;
    }
    return 0;
}

uint8_t fwUSBHostCDC::getInterfaceNumber(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].btInterfaceNumber;
    }
    return 0;
}

uint32_t fwUSBHostCDC::getBaudrate(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].iBaudrate;
    }
    return 0;
}

uint8_t fwUSBHostCDC::getStopBits(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].bStopBits;
    }
    return 0;
}

uint8_t fwUSBHostCDC::getParity(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].bParity;
    }
    return 0;
}

uint8_t fwUSBHostCDC::getDataWidth(uint8_t idx) {
    if (idx < CFG_TUH_CDC) {
        return m_sCDCInstance[idx].bDataWidth;
    }
    return 0;
}

uint8_t fwUSBHostCDC::getFirstMountedIdx() {
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++) {
        if (m_sCDCInstance[idx].bMounted) {
            return idx;
        }
    }
    return 0xFF;
}

uint8_t fwUSBHostCDC::getMountedCount() {
    uint8_t count = 0;
    for (uint8_t idx = 0; idx < CFG_TUH_CDC; idx++) {
        if (m_sCDCInstance[idx].bMounted) {
            count++;
        }
    }
    return count;
}

//------------- TinyUSB Callbacks -------------//

extern "C" {

void tuh_cdc_rx_cb(uint8_t idx) {
    obUSBHost.m_obCDC.rx(idx);
}

void tuh_cdc_mount_cb(uint8_t idx) {
    obUSBHost.m_obCDC.mount(idx);
}

void tuh_cdc_umount_cb(uint8_t idx) {
    obUSBHost.m_obCDC.unmount(idx);
}

} // extern "C"
