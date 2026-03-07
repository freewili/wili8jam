/*
 * diskio.c — FatFS low-level disk I/O bridge to SD card SPI driver
 *
 * Uses a SRAM bounce buffer for all transfers. FatFS may pass PSRAM
 * pointers (0x11000000+) directly to disk_read/disk_write for multi-
 * sector transfers. The SD SPI driver can't reliably read/write PSRAM
 * addresses because PSRAM shares the XIP/QSPI bus with flash, and
 * instruction fetches from flash contend with PSRAM data access.
 */

#include "ff.h"
#include "diskio.h"
#include "sdcard.h"
#include <string.h>

static volatile DSTATUS sd_status = STA_NOINIT;

// SRAM bounce buffer — ensures SD SPI never touches PSRAM directly
static uint8_t bounce_buf[512];

// Check if an address is in PSRAM (XIP region for external memory)
static inline int is_psram(const void *p) {
    return ((uintptr_t)p >= 0x11000000);
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;

    if (sd_init() == SD_OK) {
        sd_status = 0;
    } else {
        sd_status = STA_NOINIT;
    }
    return sd_status;
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return sd_status;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (sd_status & STA_NOINIT) return RES_NOTRDY;

    if (!is_psram(buff)) {
        // Buffer is in SRAM — direct transfer is safe
        if (sd_read_blocks(buff, (uint32_t)sector, (uint32_t)count) == SD_OK)
            return RES_OK;
        return RES_ERROR;
    }

    // Buffer is in PSRAM — read one sector at a time via SRAM bounce buffer
    for (UINT i = 0; i < count; i++) {
        if (sd_read_blocks(bounce_buf, (uint32_t)(sector + i), 1) != SD_OK)
            return RES_ERROR;
        memcpy(buff + i * 512, bounce_buf, 512);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0 || count == 0) return RES_PARERR;
    if (sd_status & STA_NOINIT) return RES_NOTRDY;

    if (!is_psram(buff)) {
        // Buffer is in SRAM — direct transfer is safe
        if (sd_write_blocks(buff, (uint32_t)sector, (uint32_t)count) == SD_OK)
            return RES_OK;
        return RES_ERROR;
    }

    // Buffer is in PSRAM — write one sector at a time via SRAM bounce buffer
    for (UINT i = 0; i < count; i++) {
        memcpy(bounce_buf, buff + i * 512, 512);
        if (sd_write_blocks(bounce_buf, (uint32_t)(sector + i), 1) != SD_OK)
            return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    if (sd_status & STA_NOINIT) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;

    case GET_SECTOR_COUNT: {
        DWORD sc = (DWORD)sd_get_sector_count();
        if (sc == 0) return RES_ERROR;
        *(DWORD *)buff = sc;
        return RES_OK;
    }

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        return RES_OK;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1; /* Unknown erase block size */
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
