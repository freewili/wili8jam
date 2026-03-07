// Created by bkidwell 1/19/26
// USB Mass Storage Class (MSC) support for fwUSBHost

#include "fwUSBHost.h"
#include "fwUSBHostMSC.h"
#include <string.h>

extern fwUSBHost obUSBHost;

fwUSBHostMSC::fwUSBHostMSC() {
}

void fwUSBHostMSC::mount(uint8_t dev_addr) {
    int slot = -1;
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (!m_sMSCInstance[i].bMounted) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return;
    
    MSCInstance& inst = m_sMSCInstance[slot];
    inst.btDevAddr = dev_addr;
    inst.bMounted = true;
    
    const uint8_t lun = 0;
    if (!tuh_msc_inquiry(dev_addr, lun, &inst.inquiry_resp, inquiry_complete_cb, 0)) {
        inst = {};
    }
}

void fwUSBHostMSC::unmount(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            m_sMSCInstance[i] = {};
            break;
        }
    }
}

bool fwUSBHostMSC::inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data) {
    fwUSBHostMSC* self = &obUSBHost.m_obMSC;
    
    int slot = -1;
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (self->m_sMSCInstance[i].bMounted && self->m_sMSCInstance[i].btDevAddr == dev_addr) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return false;
    
    MSCInstance& inst = self->m_sMSCInstance[slot];
    msc_cbw_t const* cbw = cb_data->cbw;
    msc_csw_t const* csw = cb_data->csw;

    if (csw->status != 0) {
        inst = {};
        return false;
    }

    memcpy(inst.vendor_id, inst.inquiry_resp.vendor_id, sizeof(inst.vendor_id));
    memcpy(inst.product_id, inst.inquiry_resp.product_id, sizeof(inst.product_id));
    memcpy(inst.product_rev, inst.inquiry_resp.product_rev, sizeof(inst.product_rev));

    uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
    uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

    inst.iBlockCount = block_count;
    inst.iBlockSize  = block_size;
    inst.iDiskSizeMB = (block_size > 0) ? block_count / ((1024*1024)/block_size) : 0;

    return true;
}

bool fwUSBHostMSC::isMounted(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return true;
        }
    }
    return false;
}

uint32_t fwUSBHostMSC::getDiskSizeMB(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].iDiskSizeMB;
        }
    }
    return 0;
}

uint32_t fwUSBHostMSC::getBlockCount(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].iBlockCount;
        }
    }
    return 0;
}

uint32_t fwUSBHostMSC::getBlockSize(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].iBlockSize;
        }
    }
    return 0;
}

uint8_t fwUSBHostMSC::getFirstDevAddr() {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted) {
            return m_sMSCInstance[i].btDevAddr;
        }
    }
    return 0;
}

uint8_t* fwUSBHostMSC::getVendorID(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].vendor_id;
        }
    }
    return nullptr;
}

uint8_t* fwUSBHostMSC::getProductID(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].product_id;
        }
    }
    return nullptr;
}

uint8_t* fwUSBHostMSC::getProductRev(uint8_t dev_addr) {
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted && m_sMSCInstance[i].btDevAddr == dev_addr) {
            return m_sMSCInstance[i].product_rev;
        }
    }
    return nullptr;
}

uint8_t fwUSBHostMSC::getAllDevAddrs(uint8_t* btDevAddrs) {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted) {
            btDevAddrs[count++] = m_sMSCInstance[i].btDevAddr;
        }
    }
    return count;
}

uint8_t fwUSBHostMSC::getMountedCount() {
    uint8_t count = 0;
    for (int i = 0; i < CFG_TUH_MSC; ++i) {
        if (m_sMSCInstance[i].bMounted) {
            count++;
        }
    }
    return count;
}

//------------- TinyUSB Callbacks -------------//

extern "C" {

void tuh_msc_mount_cb(uint8_t dev_addr) {
    obUSBHost.m_obMSC.mount(dev_addr);
}

void tuh_msc_umount_cb(uint8_t dev_addr) {
    obUSBHost.m_obMSC.unmount(dev_addr);
}

} // extern "C"
