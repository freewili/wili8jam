// Created 1/19/26  bkidwell
// USB Mass Storage Class (MSC) support for fwUSBHost

#ifndef FW_USB_HOST_MSC_H_
#define FW_USB_HOST_MSC_H_

#include <stdio.h>
#include "tusb.h"
#include "tusb_config.h"

struct MSCInstance {
    scsi_inquiry_resp_t inquiry_resp = {};
    bool      bMounted = false;
    uint8_t   btDevAddr = 0;
    uint32_t  iDiskSizeMB = 0;
    uint32_t  iBlockCount = 0;
    uint32_t  iBlockSize = 0;
    uint8_t   vendor_id[8] = {0};
    uint8_t   product_id[16] = {0};
    uint8_t   product_rev[4] = {0};
};

class fwUSBHostMSC {
private:
    MSCInstance m_sMSCInstance[CFG_TUH_MSC];

public:
    fwUSBHostMSC();

    void mount(uint8_t dev_addr);
    void unmount(uint8_t dev_addr);
    static bool inquiry_complete_cb(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data);

    bool isMounted(uint8_t dev_addr);
    uint32_t getDiskSizeMB(uint8_t dev_addr);
    uint32_t getBlockCount(uint8_t dev_addr);
    uint32_t getBlockSize(uint8_t dev_addr);
    uint8_t* getVendorID(uint8_t dev_addr);
    uint8_t* getProductID(uint8_t dev_addr);
    uint8_t* getProductRev(uint8_t dev_addr);

    uint8_t getFirstDevAddr();
    uint8_t getAllDevAddrs(uint8_t* btDevAddrs);
    uint8_t getMountedCount();
};

#endif // FW_USB_HOST_MSC_H_
