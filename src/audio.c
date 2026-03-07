#include "audio.h"
#include "p8_sfx.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "lauxlib.h"
#include "audio_i2s.pio.h"
#include <string.h>
#include <math.h>

// --- Pin assignments (Fruit Jam) ---
#define I2S_DIN_PIN   24
#define I2S_BCLK_PIN  26
// WS = BCLK + 1 = 27

#define I2C_SDA_PIN   20
#define I2C_SCL_PIN   21
#define CODEC_RESET_PIN 22

#define DAC_I2C_ADDR  0x18

// --- Audio parameters ---
#define SAMPLE_RATE   22050
#define BUFFER_SAMPLES 256

// PIO / DMA resources
#define AUDIO_PIO     pio1
#define AUDIO_SM      0

// --- Sine table (256 entries, int16 range) ---
static int16_t sine_table[256];

// --- Synth channel state ---
typedef struct {
    uint32_t phase;         // phase accumulator
    uint32_t phase_inc;     // frequency as phase increment
    int32_t  remaining;     // samples remaining (-1 = infinite)
    int16_t  volume;        // 0-255
    uint8_t  waveform;      // WAVE_* constant
    uint16_t noise_lfsr;    // LFSR state for noise
    bool     active;
} synth_channel_t;

static synth_channel_t channels[AUDIO_NUM_CHANNELS];

// --- DMA double buffer ---
static int32_t audio_buf[2][BUFFER_SAMPLES];  // stereo pairs packed as int32
static volatile int cur_buf;  // which buffer DMA is reading from
static int dma_ch_a, dma_ch_b;

// When true, ISR fills buffers with silence instead of mixing (avoids PSRAM access)
static volatile bool audio_paused = false;

// --- Codec I2C helpers ---

static void codec_write_reg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    i2c_write_timeout_us(i2c0, DAC_I2C_ADDR, buf, 2, false, 1000);
}

static uint8_t codec_read_reg(uint8_t reg) {
    uint8_t buf = reg;
    i2c_write_timeout_us(i2c0, DAC_I2C_ADDR, &buf, 1, true, 1000);
    i2c_read_timeout_us(i2c0, DAC_I2C_ADDR, &buf, 1, false, 1000);
    return buf;
}

static void codec_modify_reg(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t cur = codec_read_reg(reg);
    codec_write_reg(reg, (cur & ~mask) | (value & mask));
}

static void codec_set_page(uint8_t page) {
    codec_write_reg(0x00, page);
}

// --- Codec init (adapted from fruitjam-doom i_main.c) ---

static void codec_init(void) {
    // Release codec from reset
    gpio_init(CODEC_RESET_PIN);
    gpio_set_dir(CODEC_RESET_PIN, GPIO_OUT);
    gpio_put(CODEC_RESET_PIN, true);

    // I2C0 at 100kHz
    i2c_init(i2c0, 100000);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    sleep_ms(1000);

    // Soft reset
    codec_write_reg(0x01, 0x01);
    sleep_ms(10);

    // Interface: I2S, 16-bit
    codec_modify_reg(0x1B, 0xC0, 0x00);
    codec_modify_reg(0x1B, 0x30, 0x00);

    // Clock MUX: PLL from BCLK
    codec_modify_reg(0x04, 0x03, 0x03);
    codec_modify_reg(0x04, 0x0C, 0x04);

    // PLL J=0x20, D=0
    codec_write_reg(0x06, 0x20);
    codec_write_reg(0x08, 0x00);
    codec_write_reg(0x07, 0x00);

    // PLL P/R
    codec_modify_reg(0x05, 0x0F, 0x02);
    codec_modify_reg(0x05, 0x70, 0x10);

    // NDAC=8, enable
    codec_modify_reg(0x0B, 0x7F, 0x08);
    codec_modify_reg(0x0B, 0x80, 0x80);

    // MDAC=2, enable
    codec_modify_reg(0x0C, 0x7F, 0x02);
    codec_modify_reg(0x0C, 0x80, 0x80);

    // NADC=8, enable
    codec_modify_reg(0x12, 0x7F, 0x08);
    codec_modify_reg(0x12, 0x80, 0x80);

    // MADC=2, enable
    codec_modify_reg(0x13, 0x7F, 0x02);
    codec_modify_reg(0x13, 0x80, 0x80);

    // PLL power up
    codec_modify_reg(0x05, 0x80, 0x80);

    // Headset detect config
    codec_set_page(1);
    codec_modify_reg(0x2E, 0xFF, 0x0B);
    codec_set_page(0);
    codec_modify_reg(0x43, 0x80, 0x80);
    codec_modify_reg(0x30, 0x80, 0x80);
    codec_modify_reg(0x33, 0x3C, 0x14);

    // DAC power on (left + right)
    codec_modify_reg(0x3F, 0xC0, 0xC0);

    // DAC routing
    codec_set_page(1);
    codec_modify_reg(0x23, 0xC0, 0x40);
    codec_modify_reg(0x23, 0x0C, 0x04);

    // DAC volume
    codec_set_page(0);
    codec_modify_reg(0x40, 0x0C, 0x00);  // unmute
    codec_write_reg(0x41, 0x00);          // left DAC digital vol = 0dB
    codec_write_reg(0x42, 0x00);          // right DAC digital vol = 0dB

    // ADC setup
    codec_modify_reg(0x51, 0x80, 0x80);
    codec_modify_reg(0x52, 0x80, 0x00);
    codec_write_reg(0x53, 0x68);

    // HP driver + gain
    codec_set_page(1);
    codec_modify_reg(0x1F, 0xC0, 0xC0);
    codec_modify_reg(0x28, 0x04, 0x04);
    codec_modify_reg(0x29, 0x04, 0x04);
    codec_write_reg(0x24, 0x0A);
    codec_write_reg(0x25, 0x0A);
    codec_modify_reg(0x28, 0x78, 0x40);
    codec_modify_reg(0x29, 0x78, 0x40);

    // Speaker amp
    codec_modify_reg(0x20, 0x80, 0x80);
    codec_modify_reg(0x2A, 0x04, 0x04);
    codec_modify_reg(0x2A, 0x18, 0x08);
    codec_write_reg(0x26, 0x0A);

    codec_set_page(0);
}

// --- Waveform generation (called from DMA ISR — must be in RAM) ---

static inline int16_t __not_in_flash_func(synth_sample)(synth_channel_t *ch) {
    if (!ch->active) return 0;

    int16_t sample;
    uint8_t idx = ch->phase >> 24;  // top 8 bits → 256-entry index

    switch (ch->waveform) {
    case WAVE_SINE:
        sample = sine_table[idx];
        break;
    case WAVE_SQUARE:
        sample = (ch->phase & 0x80000000) ? 32767 : -32767;
        break;
    case WAVE_SAW:
        sample = (int16_t)((ch->phase >> 16) - 32768);
        break;
    case WAVE_TRIANGLE:
        if (ch->phase < 0x80000000u)
            sample = (int16_t)(((ch->phase >> 15) & 0xFFFF) - 32768);
        else
            sample = (int16_t)(32767 - (((ch->phase - 0x80000000u) >> 15) & 0xFFFF));
        break;
    case WAVE_NOISE:
        // 16-bit LFSR (taps at bits 0, 1, 5, 6 — matches PICO-8 noise)
        if (idx != ((ch->phase - ch->phase_inc) >> 24)) {
            uint16_t bit = ((ch->noise_lfsr >> 0) ^ (ch->noise_lfsr >> 1) ^
                            (ch->noise_lfsr >> 5) ^ (ch->noise_lfsr >> 6)) & 1;
            ch->noise_lfsr = (ch->noise_lfsr >> 1) | (bit << 15);
        }
        sample = (int16_t)(ch->noise_lfsr - 32768);
        break;
    default:
        sample = 0;
        break;
    }

    ch->phase += ch->phase_inc;

    // Duration countdown
    if (ch->remaining > 0) {
        ch->remaining--;
        if (ch->remaining == 0)
            ch->active = false;
    }

    return sample;
}

static void __not_in_flash_func(fill_audio_buffer)(int32_t *buf, int count) {
    if (audio_paused) {
        memset(buf, 0, count * sizeof(int32_t));
        return;
    }
    for (int i = 0; i < count; i++) {
        int32_t mix = 0;
        // Basic synth channels (audio.tone API)
        for (int c = 0; c < AUDIO_NUM_CHANNELS; c++) {
            mix += synth_sample(&channels[c]);
        }
        // PICO-8 SFX engine channels
        mix += p8_sfx_mix_sample();
        // Clip to int16
        if (mix > 32767) mix = 32767;
        if (mix < -32768) mix = -32768;
        int16_t s = (int16_t)mix;
        // Pack stereo: left in upper 16 bits, right in lower 16 (MSB first, shift left)
        buf[i] = ((int32_t)s << 16) | ((uint16_t)s);
    }
}

// --- DMA IRQ handler (ping-pong) ---

static void __not_in_flash_func(audio_dma_irq_handler)(void) {
    if (dma_irqn_get_channel_status(1, dma_ch_a)) {
        dma_irqn_acknowledge_channel(1, dma_ch_a);
        // Channel A just finished — fill buffer and reconfigure for next chain trigger
        fill_audio_buffer(audio_buf[0], BUFFER_SAMPLES);
        dma_channel_set_read_addr(dma_ch_a, audio_buf[0], false);
        dma_channel_set_trans_count(dma_ch_a, BUFFER_SAMPLES, false);
    }
    if (dma_irqn_get_channel_status(1, dma_ch_b)) {
        dma_irqn_acknowledge_channel(1, dma_ch_b);
        // Channel B just finished — fill buffer and reconfigure for next chain trigger
        fill_audio_buffer(audio_buf[1], BUFFER_SAMPLES);
        dma_channel_set_read_addr(dma_ch_b, audio_buf[1], false);
        dma_channel_set_trans_count(dma_ch_b, BUFFER_SAMPLES, false);
    }
}

// --- I2S PIO + DMA setup ---

static void i2s_init(void) {
    // Load PIO program
    uint offset = pio_add_program(AUDIO_PIO, &audio_i2s_program);
    audio_i2s_program_init(AUDIO_PIO, AUDIO_SM, offset, I2S_DIN_PIN, I2S_BCLK_PIN);

    // Clock divider: sys_clock / (22050 * 64)
    // At 126 MHz: 89.2857 → 89 + 73/256
    // At 252 MHz: 178.571 → 178 + 146/256
    {
        uint32_t sys_hz = clock_get_hz(clk_sys);
        uint32_t target = 22050 * 64; // I2S bit clock
        uint32_t div_int = sys_hz / target;
        uint32_t div_frac = ((uint64_t)(sys_hz % target) * 256) / target;
        pio_sm_set_clkdiv_int_frac(AUDIO_PIO, AUDIO_SM, div_int, div_frac);
    }

    // Claim two DMA channels for ping-pong
    dma_ch_a = dma_claim_unused_channel(true);
    dma_ch_b = dma_claim_unused_channel(true);

    // Pre-fill both buffers with silence
    memset(audio_buf, 0, sizeof(audio_buf));

    // Configure DMA channel A
    dma_channel_config cfg_a = dma_channel_get_default_config(dma_ch_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(AUDIO_PIO, AUDIO_SM, true));
    channel_config_set_chain_to(&cfg_a, dma_ch_b);  // chain to B
    dma_channel_configure(dma_ch_a, &cfg_a,
                          &AUDIO_PIO->txf[AUDIO_SM],  // write to PIO TX FIFO
                          audio_buf[0],                // read from buffer 0
                          BUFFER_SAMPLES,              // transfer count
                          false);                      // don't start yet

    // Configure DMA channel B
    dma_channel_config cfg_b = dma_channel_get_default_config(dma_ch_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(AUDIO_PIO, AUDIO_SM, true));
    channel_config_set_chain_to(&cfg_b, dma_ch_a);  // chain to A
    dma_channel_configure(dma_ch_b, &cfg_b,
                          &AUDIO_PIO->txf[AUDIO_SM],
                          audio_buf[1],
                          BUFFER_SAMPLES,
                          false);

    // Enable IRQ on both channels (DMA_IRQ_1)
    dma_irqn_set_channel_enabled(1, dma_ch_a, true);
    dma_irqn_set_channel_enabled(1, dma_ch_b, true);
    irq_set_exclusive_handler(DMA_IRQ_1, audio_dma_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);

    // Start PIO state machine
    pio_sm_set_enabled(AUDIO_PIO, AUDIO_SM, true);

    // Kick off DMA chain starting with channel A
    dma_channel_start(dma_ch_a);
}

// --- Public API ---

void audio_pause(void) {
    audio_paused = true;
}

void audio_resume(void) {
    audio_paused = false;
}

static void build_sine_table(void) {
    for (int i = 0; i < 256; i++) {
        sine_table[i] = (int16_t)(sinf((float)i * 2.0f * 3.14159265f / 256.0f) * 32767.0f);
    }
}

bool audio_init(void) {
    build_sine_table();
    memset(channels, 0, sizeof(channels));
    for (int i = 0; i < AUDIO_NUM_CHANNELS; i++) {
        channels[i].noise_lfsr = 0xACE1;
        channels[i].volume = 255;
    }

    codec_init();
    i2s_init();
    return true;
}

void audio_tone(int channel, float freq, int duration_ms, int waveform) {
    if (channel < 0 || channel >= AUDIO_NUM_CHANNELS) return;
    if (waveform < 0 || waveform > WAVE_NOISE) waveform = WAVE_SQUARE;

    synth_channel_t *ch = &channels[channel];
    ch->phase = 0;
    ch->phase_inc = (uint32_t)(freq * 4294967296.0f / (float)SAMPLE_RATE);
    ch->waveform = (uint8_t)waveform;
    ch->remaining = (duration_ms > 0) ? (int32_t)((duration_ms * SAMPLE_RATE) / 1000) : -1;
    ch->noise_lfsr = 0xACE1;
    ch->active = true;
}

void audio_stop(int channel) {
    if (channel < 0) {
        for (int i = 0; i < AUDIO_NUM_CHANNELS; i++)
            channels[i].active = false;
    } else if (channel < AUDIO_NUM_CHANNELS) {
        channels[channel].active = false;
    }
}

void audio_volume(int level) {
    if (level < 0) level = 0;
    if (level > 7) level = 7;

    // Map 0-7 to DAC digital volume register values
    // Register range: 0x00 = 0dB, each step +0.5dB, negative = attenuation
    // We map: 7=0dB(0x00), 6=-4dB(0xF8), 5=-8dB(0xF0), ... 0=mute(0x80)
    static const uint8_t vol_map[8] = {
        0x80,  // 0: mute
        0xE0,  // 1: -16dB
        0xE8,  // 2: -12dB
        0xF0,  // 3: -8dB
        0xF4,  // 4: -6dB
        0xF8,  // 5: -4dB
        0xFC,  // 6: -2dB
        0x00,  // 7: 0dB
    };

    uint8_t vol = vol_map[level];
    codec_set_page(0);
    codec_write_reg(0x41, vol);  // Left DAC volume
    codec_write_reg(0x42, vol);  // Right DAC volume
}

// --- Lua bindings ---

// audio.tone(freq, [duration_ms], [waveform], [channel])
static int l_audio_tone(lua_State *L) {
    float freq = (float)luaL_checknumber(L, 1);
    int duration = (int)luaL_optnumber(L, 2, 0);
    int waveform = (int)luaL_optnumber(L, 3, WAVE_SQUARE);
    int channel = (int)luaL_optnumber(L, 4, 0);
    audio_tone(channel, freq, duration, waveform);
    return 0;
}

// audio.stop([channel])
static int l_audio_stop(lua_State *L) {
    int channel = (int)luaL_optnumber(L, 1, -1);
    audio_stop(channel);
    return 0;
}

// audio.volume(level)
static int l_audio_volume(lua_State *L) {
    int level = (int)luaL_checknumber(L, 1);
    audio_volume(level);
    return 0;
}

static const luaL_Reg audiolib[] = {
    {"tone",   l_audio_tone},
    {"stop",   l_audio_stop},
    {"volume", l_audio_volume},
    {NULL, NULL}
};

int luaopen_audio(lua_State *L) {
    luaL_newlib(L, audiolib);
    return 1;
}
