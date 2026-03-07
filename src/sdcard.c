/*
 * sdcard.c — SD card SPI driver for Adafruit Fruit Jam (RP2350B)
 *
 * Standard SPI-mode SD protocol implementation using Pico SDK hardware_spi.
 * Pin assignments from adafruit_fruit_jam.h:
 *   SPI0: SCK=GPIO34, MOSI=GPIO35, MISO=GPIO36, CS=GPIO39
 *   Card detect: GPIO33
 */

#include "sdcard.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

/* Fruit Jam SD card pins */
#define SD_SPI       spi0
#define SD_SCK_PIN   34
#define SD_MOSI_PIN  35
#define SD_MISO_PIN  36
#define SD_CS_PIN    39
#define SD_CD_PIN    33

/* SPI speeds */
#define SD_SPI_INIT_HZ   400000     /* 400 kHz for init */
#define SD_SPI_FAST_HZ   12500000   /* 12.5 MHz after init */

/* SD commands */
#define CMD0    0   /* GO_IDLE_STATE */
#define CMD1    1   /* SEND_OP_COND (MMC) */
#define CMD8    8   /* SEND_IF_COND */
#define CMD9    9   /* SEND_CSD */
#define CMD12   12  /* STOP_TRANSMISSION */
#define CMD16   16  /* SET_BLOCKLEN */
#define CMD17   17  /* READ_SINGLE_BLOCK */
#define CMD18   18  /* READ_MULTIPLE_BLOCK */
#define CMD24   24  /* WRITE_BLOCK */
#define CMD25   25  /* WRITE_MULTIPLE_BLOCK */
#define CMD55   55  /* APP_CMD */
#define CMD58   58  /* READ_OCR */
#define ACMD41  41  /* SD_SEND_OP_COND */

static int card_type = SD_TYPE_NONE;

static inline void cs_select(void) {
    gpio_put(SD_CS_PIN, 0);
}

static inline void cs_deselect(void) {
    gpio_put(SD_CS_PIN, 1);
}

static void spi_send_byte(uint8_t b) {
    spi_write_blocking(SD_SPI, &b, 1);
}

static uint8_t spi_recv_byte(void) {
    uint8_t r;
    uint8_t tx = 0xFF;
    spi_write_read_blocking(SD_SPI, &tx, &r, 1);
    return r;
}

/* Send a command and return the R1 response byte */
static uint8_t sd_send_cmd(uint8_t cmd, uint32_t arg) {
    uint8_t buf[6];
    uint8_t r;

    /* For CMD0 and CMD8 we need valid CRC; others can use dummy */
    buf[0] = 0x40 | cmd;
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)(arg);

    if (cmd == CMD0)
        buf[5] = 0x95;  /* Valid CRC for CMD0(0) */
    else if (cmd == CMD8)
        buf[5] = 0x87;  /* Valid CRC for CMD8(0x1AA) */
    else
        buf[5] = 0x01;  /* Dummy CRC + stop bit */

    /* Send the command */
    spi_write_blocking(SD_SPI, buf, 6);

    /* Wait for response (not 0xFF), up to 10 bytes */
    for (int i = 0; i < 10; i++) {
        r = spi_recv_byte();
        if ((r & 0x80) == 0) return r;
    }
    return 0xFF; /* Timeout */
}

/* Send ACMD (CMD55 + CMDn) */
static uint8_t sd_send_acmd(uint8_t cmd, uint32_t arg) {
    sd_send_cmd(CMD55, 0);
    return sd_send_cmd(cmd, arg);
}

/* Wait for card to be ready (not busy) */
static bool sd_wait_ready(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t r;
    do {
        r = spi_recv_byte();
        if (r == 0xFF) return true;
    } while (!time_reached(deadline));
    return false;
}

/* Wait for a data token, return true if received */
static bool sd_wait_data_token(uint8_t token, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    uint8_t r;
    do {
        r = spi_recv_byte();
        if (r == token) return true;
        if (r != 0xFF) return false; /* Error token */
    } while (!time_reached(deadline));
    return false;
}

int sd_init(void) {
    card_type = SD_TYPE_NONE;

    /* Init CS pin as GPIO output, deselected (high) */
    gpio_init(SD_CS_PIN);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    cs_deselect();

    /* Init card detect pin with pull-up */
    gpio_init(SD_CD_PIN);
    gpio_set_dir(SD_CD_PIN, GPIO_IN);
    gpio_pull_up(SD_CD_PIN);

    /* Init SPI at slow speed for card init */
    spi_init(SD_SPI, SD_SPI_INIT_HZ);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);

    /* Send 80+ clock pulses with CS high (card power-up sequence) */
    cs_deselect();
    for (int i = 0; i < 10; i++) {
        spi_send_byte(0xFF);
    }

    /* CMD0: GO_IDLE_STATE — enter SPI mode */
    cs_select();
    uint8_t r1;
    int retry;

    for (retry = 0; retry < 5; retry++) {
        r1 = sd_send_cmd(CMD0, 0);
        if (r1 == 0x01) break;
        /* Extra clocks between retries */
        spi_send_byte(0xFF);
    }
    cs_deselect();
    spi_send_byte(0xFF);

    if (r1 != 0x01) {
        return SD_NO_CARD;
    }

    /* CMD8: SEND_IF_COND — check for SDv2 */
    cs_select();
    r1 = sd_send_cmd(CMD8, 0x000001AA);
    if (r1 == 0x01) {
        /* SDv2 card — read 4 bytes of R7 response */
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++)
            ocr[i] = spi_recv_byte();
        cs_deselect();
        spi_send_byte(0xFF);

        /* Check voltage accepted and echo pattern */
        if (ocr[2] != 0x01 || ocr[3] != 0xAA) {
            return SD_ERROR;
        }

        /* ACMD41 with HCS bit — wait for card ready */
        absolute_time_t deadline = make_timeout_time_ms(2000);
        do {
            cs_select();
            r1 = sd_send_acmd(ACMD41, 0x40000000);
            cs_deselect();
            spi_send_byte(0xFF);
            if (r1 == 0x00) break;
        } while (!time_reached(deadline));

        if (r1 != 0x00) {
            return SD_TIMEOUT;
        }

        /* CMD58: READ_OCR — check CCS bit for SDHC */
        cs_select();
        r1 = sd_send_cmd(CMD58, 0);
        if (r1 == 0x00) {
            uint8_t ocr2[4];
            for (int i = 0; i < 4; i++)
                ocr2[i] = spi_recv_byte();
            card_type = (ocr2[0] & 0x40) ? SD_TYPE_SDHC : SD_TYPE_SDv2;
        }
        cs_deselect();
        spi_send_byte(0xFF);
    } else {
        /* SDv1 or MMC */
        cs_deselect();
        spi_send_byte(0xFF);

        /* Try ACMD41 for SDv1 */
        cs_select();
        r1 = sd_send_acmd(ACMD41, 0);
        cs_deselect();
        spi_send_byte(0xFF);

        if (r1 <= 0x01) {
            card_type = SD_TYPE_SDv1;
            absolute_time_t deadline = make_timeout_time_ms(2000);
            do {
                cs_select();
                r1 = sd_send_acmd(ACMD41, 0);
                cs_deselect();
                spi_send_byte(0xFF);
                if (r1 == 0x00) break;
            } while (!time_reached(deadline));
        } else {
            /* MMC — not supported */
            return SD_ERROR;
        }

        if (r1 != 0x00) {
            return SD_TIMEOUT;
        }
    }

    /* For non-SDHC cards, set block length to 512 */
    if (card_type != SD_TYPE_SDHC) {
        cs_select();
        r1 = sd_send_cmd(CMD16, 512);
        cs_deselect();
        spi_send_byte(0xFF);
        if (r1 != 0x00) {
            return SD_ERROR;
        }
    }

    /* Switch to fast SPI clock */
    spi_set_baudrate(SD_SPI, SD_SPI_FAST_HZ);

    return SD_OK;
}

int sd_read_blocks(uint8_t *buf, uint32_t sector, uint32_t count) {
    /* SDv1/v2 use byte addressing, SDHC uses block addressing */
    uint32_t addr = (card_type == SD_TYPE_SDHC) ? sector : sector * 512;

    if (count == 1) {
        /* Single block read */
        cs_select();
        if (sd_send_cmd(CMD17, addr) != 0x00) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_ERROR;
        }
        if (!sd_wait_data_token(0xFE, 500)) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_TIMEOUT;
        }
        spi_read_blocking(SD_SPI, 0xFF, buf, 512);
        /* Discard CRC */
        spi_recv_byte();
        spi_recv_byte();
        cs_deselect();
        spi_send_byte(0xFF);
    } else {
        /* Multi-block read */
        cs_select();
        if (sd_send_cmd(CMD18, addr) != 0x00) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_ERROR;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!sd_wait_data_token(0xFE, 500)) {
                sd_send_cmd(CMD12, 0);
                cs_deselect();
                spi_send_byte(0xFF);
                return SD_TIMEOUT;
            }
            spi_read_blocking(SD_SPI, 0xFF, buf + i * 512, 512);
            spi_recv_byte();
            spi_recv_byte();
        }
        /* Stop transmission */
        sd_send_cmd(CMD12, 0);
        cs_deselect();
        spi_send_byte(0xFF);
    }

    return SD_OK;
}

int sd_write_blocks(const uint8_t *buf, uint32_t sector, uint32_t count) {
    uint32_t addr = (card_type == SD_TYPE_SDHC) ? sector : sector * 512;

    if (count == 1) {
        cs_select();
        if (sd_send_cmd(CMD24, addr) != 0x00) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_ERROR;
        }
        if (!sd_wait_ready(500)) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_TIMEOUT;
        }
        /* Data token for single block write */
        spi_send_byte(0xFE);
        spi_write_blocking(SD_SPI, buf, 512);
        /* Dummy CRC */
        spi_send_byte(0xFF);
        spi_send_byte(0xFF);
        /* Check data response */
        uint8_t resp = spi_recv_byte();
        if ((resp & 0x1F) != 0x05) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_ERROR;
        }
        /* Wait for write to complete */
        if (!sd_wait_ready(1000)) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_TIMEOUT;
        }
        cs_deselect();
        spi_send_byte(0xFF);
    } else {
        cs_select();
        if (sd_send_cmd(CMD25, addr) != 0x00) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_ERROR;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (!sd_wait_ready(500)) {
                cs_deselect();
                spi_send_byte(0xFF);
                return SD_TIMEOUT;
            }
            /* Data token for multi-block write */
            spi_send_byte(0xFC);
            spi_write_blocking(SD_SPI, buf + i * 512, 512);
            spi_send_byte(0xFF);
            spi_send_byte(0xFF);
            uint8_t resp = spi_recv_byte();
            if ((resp & 0x1F) != 0x05) {
                cs_deselect();
                spi_send_byte(0xFF);
                return SD_ERROR;
            }
        }
        /* Stop token */
        if (!sd_wait_ready(500)) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_TIMEOUT;
        }
        spi_send_byte(0xFD);
        /* Wait for card to finish */
        if (!sd_wait_ready(1000)) {
            cs_deselect();
            spi_send_byte(0xFF);
            return SD_TIMEOUT;
        }
        cs_deselect();
        spi_send_byte(0xFF);
    }

    return SD_OK;
}

uint32_t sd_get_sector_count(void) {
    uint8_t csd[16];

    cs_select();
    if (sd_send_cmd(CMD9, 0) != 0x00) {
        cs_deselect();
        spi_send_byte(0xFF);
        return 0;
    }
    if (!sd_wait_data_token(0xFE, 500)) {
        cs_deselect();
        spi_send_byte(0xFF);
        return 0;
    }
    spi_read_blocking(SD_SPI, 0xFF, csd, 16);
    spi_recv_byte();
    spi_recv_byte();
    cs_deselect();
    spi_send_byte(0xFF);

    uint32_t sectors = 0;
    if ((csd[0] >> 6) == 1) {
        /* CSD v2 (SDHC/SDXC) */
        uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) |
                          ((uint32_t)csd[8] << 8) |
                          (uint32_t)csd[9];
        sectors = (c_size + 1) * 1024;
    } else {
        /* CSD v1 */
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03) << 10) |
                          ((uint32_t)csd[7] << 2) |
                          ((uint32_t)(csd[8] >> 6));
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03) << 1) |
                               ((uint32_t)(csd[10] >> 7));
        uint32_t read_bl_len = csd[5] & 0x0F;
        sectors = (c_size + 1) << (c_size_mult + 2 + read_bl_len - 9);
    }

    return sectors;
}

int sd_get_type(void) {
    return card_type;
}
