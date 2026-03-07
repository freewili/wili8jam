#ifndef P8_SFX_H
#define P8_SFX_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the SFX/music engine. Call once after audio_init().
void p8_sfx_init(void);

// Play SFX n (0-63) on channel (-1=auto, 0-3=specific).
// offset: starting note (0-31), length: notes to play (0=all remaining).
void p8_sfx_play(int n, int channel, int offset, int length);

// Stop SFX. channel: 0-3=specific, -1=stop all, -2=stop all music-owned channels.
void p8_sfx_stop(int channel);

// Start music at pattern n (0-63). music(-1) stops.
// fade_len: fade-out time in ms (0=instant). channel_mask: bitmask of channels music may use (0xF=all).
void p8_music_play(int n, int fade_len, int channel_mask);

// Stop music playback.
void p8_music_stop(void);

// Generate one mixed audio sample from all 4 SFX channels.
// Called from DMA ISR — must be in RAM.
int16_t p8_sfx_mix_sample(void);

// Query SFX state for stat()
int p8_sfx_get_current(int channel);      // SFX number playing on channel (-1 if idle)
int p8_sfx_get_note(int channel);         // Current note index on channel
int p8_music_get_pattern(void);           // Current music pattern (-1 if not playing)
int p8_music_get_count(void);             // Number of patterns played since last music() call

#ifdef __cplusplus
}
#endif

#endif // P8_SFX_H
