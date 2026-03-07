#ifndef SDCARD_H
#define SDCARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
#define SD_OK        0
#define SD_ERROR     1
#define SD_TIMEOUT   2
#define SD_NO_CARD   3

/* Card types */
#define SD_TYPE_NONE  0
#define SD_TYPE_SDv1  1
#define SD_TYPE_SDv2  2
#define SD_TYPE_SDHC  3

/* Initialize the SD card over SPI. Returns SD_OK on success. */
int sd_init(void);

/* Read 'count' 512-byte sectors starting at 'sector' into 'buf'. */
int sd_read_blocks(uint8_t *buf, uint32_t sector, uint32_t count);

/* Write 'count' 512-byte sectors from 'buf' starting at 'sector'. */
int sd_write_blocks(const uint8_t *buf, uint32_t sector, uint32_t count);

/* Get total number of 512-byte sectors on the card. Returns 0 on error. */
uint32_t sd_get_sector_count(void);

/* Get card type (SD_TYPE_*) */
int sd_get_type(void);

#ifdef __cplusplus
}
#endif

#endif /* SDCARD_H */
