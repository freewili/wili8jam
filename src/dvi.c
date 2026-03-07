/*
 * DVI output driver for Adafruit Fruit Jam (RP2350B) via HSTX.
 * Adapted from MicroPython/Adafruit picodvi (MIT License).
 *
 * Outputs 640x480@60Hz DVI from a 128x128 RGB565 framebuffer.
 * 3x pixel scaling (384x384) centered with black letterbox borders.
 * Fully DMA-driven — no CPU involvement after init.
 */

#include "dvi.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "hardware/clocks.h"

// --- TMDS control symbols ---
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))

// --- 640x480 @ 60Hz timing ---
#define H_FRONT_PORCH   16
#define H_SYNC_WIDTH    96
#define H_BACK_PORCH    48
#define H_ACTIVE_PIXELS 640

#define V_FRONT_PORCH   10
#define V_SYNC_WIDTH    2
#define V_BACK_PORCH    33
#define V_ACTIVE_LINES  480

#define V_TOTAL_LINES (V_FRONT_PORCH + V_SYNC_WIDTH + V_BACK_PORCH + V_ACTIVE_LINES)

// --- HSTX command types ---
#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// --- Framebuffer and scaling ---
#define FB_WIDTH  128
#define FB_HEIGHT 128
#define OUTPUT_SCALING 3

// --- HSTX command lists for blanking and active lines ---
#define VSYNC_LEN  6

static uint32_t vblank_vsync_off[VSYNC_LEN] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
    SYNC_V1_H1
};

static uint32_t vblank_vsync_on[VSYNC_LEN] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V0_H1,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V0_H0,
    HSTX_CMD_RAW_REPEAT | (H_BACK_PORCH + H_ACTIVE_PIXELS),
    SYNC_V0_H1
};

// Horizontal/vertical borders for centered 3x display
#define H_BORDER ((H_ACTIVE_PIXELS - FB_WIDTH * OUTPUT_SCALING) / 2)  // 128
#define V_BORDER ((V_ACTIVE_LINES - FB_HEIGHT * OUTPUT_SCALING) / 2)  // 48

// Content line part 1: preamble + left border + TMDS pixel command
// (sent BEFORE pixel data DMA — TMDS command will consume pixels from next DMA)
#define VACTIVE_PRE_LEN 11
static uint32_t vactive_pre[VACTIVE_PRE_LEN] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | H_BORDER,               // left border (128 black pixels)
    0x00000000,                                      // black pixel data
    HSTX_CMD_TMDS | (FB_WIDTH * OUTPUT_SCALING)     // 384 active pixels (128 input x 3)
};

// Content line part 2: right border (sent AFTER pixel data DMA)
#define VACTIVE_POST_LEN 2
static uint32_t vactive_post[VACTIVE_POST_LEN] = {
    HSTX_CMD_TMDS_REPEAT | H_BORDER,               // right border (128 black pixels)
    0x00000000                                       // black pixel data
};

// Black line for top/bottom letterbox borders
#define VACTIVE_BLACK_LEN 10
static uint32_t vactive_black[VACTIVE_BLACK_LEN] = {
    HSTX_CMD_RAW_REPEAT | H_FRONT_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_SYNC_WIDTH,
    SYNC_V1_H0,
    HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | H_BACK_PORCH,
    SYNC_V1_H1,
    HSTX_CMD_TMDS_REPEAT | H_ACTIVE_PIXELS,        // 640 black pixels
    0x00000000                                       // black pixel data
};

// --- DMA state ---
#define DMA_IRQ_HSTX DMA_IRQ_2

static int dma_pixel_channel = -1;
static int dma_command_channel = -1;

// DMA command buffer: each scanline needs 4 words (ctrl, write_addr, count, read_addr).
// Content lines need 3 transfers (pre-cmd, pixels, post-cmd) = 12 words each.
// Other lines need 1 transfer = 4 words each.
// Total: (V_TOTAL_LINES - content_lines) * 4 + content_lines * 12 + 4
#define CONTENT_LINES (FB_HEIGHT * OUTPUT_SCALING)
#define DMA_CMD_BUF_WORDS ((V_TOTAL_LINES - CONTENT_LINES) * 4 + CONTENT_LINES * 12 + 4)
static uint32_t dma_commands[DMA_CMD_BUF_WORDS];

static void __not_in_flash_func(dma_irq_handler)(void) {
    dma_hw->intr = 1u << dma_pixel_channel;
    // Restart command channel from beginning of command list
    dma_channel_hw_t *ch = &dma_hw->ch[dma_command_channel];
    ch->al3_read_addr_trig = (uintptr_t)dma_commands;
}

// Fruit Jam HSTX pins
#define HSTX_FIRST_PIN 12
#define PIN_CKP  13
#define PIN_D2P  19  // Red
#define PIN_D1P  17  // Green
#define PIN_D0P  15  // Blue

void dvi_init(uint16_t *framebuffer) {
    // HSTX clock must be 126 MHz for correct DVI TMDS bit rate (252 Mbit/s = 2 bits/cycle).
    // When sys_clk is overclocked, divide it down to 126 MHz for HSTX.
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint32_t hstx_hz = 126000000;
    clock_configure(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    sys_hz, hstx_hz);

    // Claim DMA channels
    dma_pixel_channel = dma_claim_unused_channel(true);
    dma_command_channel = dma_claim_unused_channel(true);

    // Build DMA command list for one frame
    // With pixel scaling > 1, each line needs 4 words for the command channel entry,
    // and active lines need an additional 4 words for the pixel data entry.
    uint32_t dma_ctrl_base = (uint32_t)dma_command_channel << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB |
        DREQ_HSTX << DMA_CH0_CTRL_TRIG_TREQ_SEL_LSB |
        DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS |
        DMA_CH0_CTRL_TRIG_INCR_READ_BITS |
        DMA_CH0_CTRL_TRIG_EN_BITS;

    // For pixel scaling with 16-bit color: use DMA_SIZE_16 (no BSWAP needed
    // since our palette stores native little-endian RGB565)
    uint32_t dma_pixel_ctrl = dma_ctrl_base |
        DMA_SIZE_16 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB;
    uint32_t dma_ctrl = dma_ctrl_base |
        DMA_SIZE_32 << DMA_CH0_CTRL_TRIG_DATA_SIZE_LSB;

    uint32_t dma_write_addr = (uint32_t)&hstx_fifo_hw->fifo;

    // Compute scanline regions
    size_t vsync_start = 0;
    size_t vsync_end = V_SYNC_WIDTH;
    size_t backporch_end = vsync_end + V_BACK_PORCH;
    size_t active_start = backporch_end;
    size_t frontporch_start = V_TOTAL_LINES - V_FRONT_PORCH;
    size_t content_start = active_start + V_BORDER;
    size_t content_end = content_start + FB_HEIGHT * OUTPUT_SCALING;

    size_t cw = 0;
    for (size_t line = 0; line < (size_t)V_TOTAL_LINES; line++) {
        // Every line writes ctrl + write_addr for pixel scaling
        dma_commands[cw++] = dma_ctrl;
        dma_commands[cw++] = dma_write_addr;

        if (line >= vsync_start && line < vsync_end) {
            dma_commands[cw++] = VSYNC_LEN;
            dma_commands[cw++] = (uintptr_t)vblank_vsync_on;
        } else if (line >= active_start && line < frontporch_start) {
            if (line >= content_start && line < content_end) {
                // Content line: 3 DMA transfers
                // Transfer 1: preamble + left border + TMDS command
                dma_commands[cw++] = VACTIVE_PRE_LEN;
                dma_commands[cw++] = (uintptr_t)vactive_pre;

                // Transfer 2: pixel data (consumed by TMDS command from transfer 1)
                size_t row = (line - content_start) / OUTPUT_SCALING;
                dma_commands[cw++] = dma_pixel_ctrl;
                dma_commands[cw++] = dma_write_addr;
                dma_commands[cw++] = FB_WIDTH;
                dma_commands[cw++] = (uintptr_t)&framebuffer[row * FB_WIDTH];

                // Transfer 3: right border
                dma_commands[cw++] = dma_ctrl;
                dma_commands[cw++] = dma_write_addr;
                dma_commands[cw++] = VACTIVE_POST_LEN;
                dma_commands[cw++] = (uintptr_t)vactive_post;
            } else {
                // Top/bottom letterbox border (all black)
                dma_commands[cw++] = VACTIVE_BLACK_LEN;
                dma_commands[cw++] = (uintptr_t)vactive_black;
            }
        } else {
            // Front porch or back porch (non-active blanking)
            dma_commands[cw++] = VSYNC_LEN;
            dma_commands[cw++] = (uintptr_t)vblank_vsync_off;
        }
    }

    // Terminator: trigger IRQ on pixel channel completion
    dma_commands[cw++] = DMA_CH0_CTRL_TRIG_IRQ_QUIET_BITS | DMA_CH0_CTRL_TRIG_EN_BITS;
    dma_commands[cw++] = 0;
    dma_commands[cw++] = 0;
    dma_commands[cw++] = 0;

    // Configure HSTX TMDS encoder for RGB565.
    // RP2350 HSTX extracts NBITS+1 bits starting from BIT 7 of rotated data.
    // RGB565 layout: R[15:11] G[10:5] B[4:0]
    // ROT right-rotates so the MSB of each component lands at bit 7.
    hstx_ctrl_hw->expand_tmds =
        4  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |  // Red: 5 bits
        8  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |  // Red: bit 15 → bit 7
        5  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |  // Green: 6 bits
        3  << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |  // Green: bit 10 → bit 7
        4  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |  // Blue: 5 bits
        29 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;     // Blue: bit 4 → bit 7

    // With pixel scaling: 1 pixel per word, repeated 3x by HSTX shift register
    hstx_ctrl_hw->expand_shift =
        OUTPUT_SCALING << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        16             << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB   |
        1              << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0              << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    // Serial output config: pop every 5 shifts, shift 2 bits/cycle.
    // CLKDIV based on actual HSTX clock (always 126 MHz → div 5).
    uint32_t clkdiv = hstx_hz / 25200000;
    if (clkdiv < 1) clkdiv = 1;
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        clkdiv << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // Assign HSTX clock pair
    {
        int bit = PIN_CKP - HSTX_FIRST_PIN;
        hstx_ctrl_hw->bit[bit    ] = HSTX_CTRL_BIT0_CLK_BITS;
        hstx_ctrl_hw->bit[bit ^ 1] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    }

    // TMDS lanes: L0=Blue, L1=Green, L2=Red
    // Fruit Jam: D0P(pin15)→DVI Blue, D1P(pin17)→DVI Green, D2P(pin19)→DVI Red
    const int pinout[] = { PIN_D0P, PIN_D1P, PIN_D2P };
    for (uint lane = 0; lane < 3; ++lane) {
        int bit = pinout[lane] - HSTX_FIRST_PIN;
        uint32_t lane_data_sel_bits =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        hstx_ctrl_hw->bit[bit    ] = lane_data_sel_bits;
        hstx_ctrl_hw->bit[bit ^ 1] = lane_data_sel_bits | HSTX_CTRL_BIT0_INV_BITS;
    }

    // Set all HSTX pins to HSTX function
    for (int i = 12; i <= 19; ++i) {
        gpio_set_function(i, GPIO_FUNC_HSTX);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_4MA);
    }

    // Configure command channel: writes to pixel channel's control registers
    dma_channel_config c = dma_channel_get_default_config(dma_command_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    // Wrap at 16 bytes (4 words) — writes ctrl, write_addr, trans_count, read_addr
    channel_config_set_ring(&c, true, 4);
    dma_channel_configure(
        dma_command_channel,
        &c,
        &dma_hw->ch[dma_pixel_channel].al3_ctrl,
        dma_commands,
        4, // 16 bytes / 4 = 4 words per transfer
        false
    );

    // Set up interrupt on pixel channel completion
    dma_hw->irq_ctrl[DMA_IRQ_HSTX - DMA_IRQ_0].ints = (1u << dma_pixel_channel);
    dma_hw->irq_ctrl[DMA_IRQ_HSTX - DMA_IRQ_0].inte = (1u << dma_pixel_channel);
    irq_set_exclusive_handler(DMA_IRQ_HSTX, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_HSTX, true);
    irq_set_priority(DMA_IRQ_HSTX, PICO_HIGHEST_IRQ_PRIORITY);

    // Give DMA bus priority
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    // Start the first frame
    dma_irq_handler();
}
