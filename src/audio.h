#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// Waveform types
#define WAVE_SINE     0
#define WAVE_SQUARE   1
#define WAVE_SAW      2
#define WAVE_TRIANGLE 3
#define WAVE_NOISE    4

#define AUDIO_NUM_CHANNELS 4

// Initialize codec (I2C), I2S (PIO), and DMA. Call once after DVI init.
bool audio_init(void);

// Play a tone on a synth channel (0-3).
// freq: Hz, duration_ms: 0 = infinite, waveform: WAVE_* constant
void audio_tone(int channel, float freq, int duration_ms, int waveform);

// Stop a channel, or all channels if channel < 0.
void audio_stop(int channel);

// Set DAC volume level (0-7). Writes to codec registers via I2C.
void audio_volume(int level);

// Pause/resume audio mixing. While paused, the DMA ISR outputs silence
// instead of reading PSRAM. Use around SD card operations to prevent
// QSPI bus contention that can corrupt the SD card.
void audio_pause(void);
void audio_resume(void);

// Lua library opener
int luaopen_audio(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_H
