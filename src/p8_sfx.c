/*
 * PICO-8 SFX + Music Engine
 *
 * Decodes SFX data from PICO-8 virtual memory (0x3200-0x42FF),
 * implements 8 waveforms and 7 effects, and drives the existing
 * I2S audio system via the DMA callback.
 *
 * Music pattern sequencer reads from 0x3100-0x31FF and triggers
 * SFX playback on the 4 audio channels.
 */

#include "p8_sfx.h"
#include "p8_api.h"
#include "pico/stdlib.h"
#include <string.h>
#include <math.h>

// Cached pointer to PICO-8 virtual memory — set once at init,
// avoids calling flash-resident p8_get_memory() from DMA ISR.
static uint8_t *p8_mem_cached;

// Memory map offsets (must match p8_api.c)
#define P8_SFX_BASE   0x3200
#define P8_MUSIC_BASE 0x3100
#define P8_SFX_SIZE   68    // bytes per SFX slot
#define P8_SFX_NOTES  32
#define P8_SFX_SLOTS  64

#define SAMPLE_RATE 22050
// 1 tick = 1/128 second in samples
#define SAMPLES_PER_TICK (SAMPLE_RATE / 128)  // ~172

// ============================================================
// PICO-8 Note Decoding (from virtual memory)
// ============================================================

// In memory, each note is a 16-bit LE word:
//   bit 0:     custom flag
//   bits 1-6:  pitch (0-63)
//   bits 7-9:  waveform (0-7)
//   bits 10-12: volume (0-7)
//   bits 13-15: effect (0-7)

typedef struct {
    uint8_t pitch;      // 0-63
    uint8_t waveform;   // 0-7
    uint8_t volume;     // 0-7
    uint8_t effect;     // 0-7
    uint8_t custom;     // 0 or 1
} p8_note_t;

static __always_inline p8_note_t decode_note(const uint8_t *mem, int sfx, int note) {
    int addr = P8_SFX_BASE + sfx * P8_SFX_SIZE + note * 2;
    uint16_t w = (uint16_t)mem[addr] | ((uint16_t)mem[addr + 1] << 8);
    p8_note_t n;
    n.custom   = (w >> 0) & 1;
    n.pitch    = (w >> 1) & 0x3F;
    n.waveform = (w >> 7) & 7;
    n.volume   = (w >> 10) & 7;
    n.effect   = (w >> 13) & 7;
    return n;
}

static __always_inline uint8_t sfx_speed(const uint8_t *mem, int sfx) {
    return mem[P8_SFX_BASE + sfx * P8_SFX_SIZE + 65];
}

static __always_inline uint8_t sfx_loop_start(const uint8_t *mem, int sfx) {
    return mem[P8_SFX_BASE + sfx * P8_SFX_SIZE + 66];
}

static __always_inline uint8_t sfx_loop_end(const uint8_t *mem, int sfx) {
    return mem[P8_SFX_BASE + sfx * P8_SFX_SIZE + 67];
}

// ============================================================
// Pitch → Frequency / Phase Increment
// ============================================================

// PICO-8 note 0 = C-2 (65.41 Hz), 12 semitones per octave
static uint32_t pitch_to_phase_inc[64];

// Precomputed vibrato tables: phase_inc at ±0.5 semitone for each pitch
// 2^(-0.5/12) ≈ 0.97153, 2^(+0.5/12) ≈ 1.02930
static uint32_t pitch_phase_inc_lo[64];
static uint32_t pitch_phase_inc_hi[64];

static void build_pitch_table(void) {
    for (int i = 0; i < 64; i++) {
        float freq = 65.40639f * powf(2.0f, (float)i / 12.0f);
        pitch_to_phase_inc[i] = (uint32_t)(freq * 4294967296.0f / (float)SAMPLE_RATE);
        // Vibrato: ±0.5 semitone
        float flo = freq * 0.97153f; // 2^(-0.5/12)
        float fhi = freq * 1.02930f; // 2^(+0.5/12)
        pitch_phase_inc_lo[i] = (uint32_t)(flo * 4294967296.0f / (float)SAMPLE_RATE);
        pitch_phase_inc_hi[i] = (uint32_t)(fhi * 4294967296.0f / (float)SAMPLE_RATE);
    }
}

// ============================================================
// Waveform Generation
// ============================================================

// Wavetable: 256 entries, int16 range
static int16_t wt_triangle[256];
static int16_t wt_tilted_saw[256];
static int16_t wt_saw[256];
static int16_t wt_square[256];
static int16_t wt_pulse[256];
static int16_t wt_organ[256];
static int16_t wt_phaser[256];

static void build_wavetables(void) {
    for (int i = 0; i < 256; i++) {
        float t = (float)i / 256.0f;  // 0.0 to ~1.0

        // 0: Triangle
        if (t < 0.25f)
            wt_triangle[i] = (int16_t)(t * 4.0f * 32767.0f);
        else if (t < 0.75f)
            wt_triangle[i] = (int16_t)((1.0f - (t - 0.25f) * 4.0f) * 32767.0f);
        else
            wt_triangle[i] = (int16_t)(((t - 0.75f) * 4.0f - 1.0f) * 32767.0f);

        // 1: Tilted saw (ramp up 7/8, sharp drop 1/8)
        if (t < 0.875f)
            wt_tilted_saw[i] = (int16_t)((t / 0.875f * 2.0f - 1.0f) * 32767.0f);
        else
            wt_tilted_saw[i] = (int16_t)((1.0f - (t - 0.875f) / 0.125f * 2.0f) * 32767.0f);

        // 2: Saw (downward)
        wt_saw[i] = (int16_t)((1.0f - t * 2.0f) * 32767.0f);

        // 3: Square (50% duty)
        wt_square[i] = (t < 0.5f) ? 32767 : -32767;

        // 4: Pulse (25% duty)
        wt_pulse[i] = (t < 0.25f) ? 32767 : -32767;

        // 5: Organ (triangle harmonics: fundamental + 2nd harmonic at half amp)
        {
            float v = 0.0f;
            // Fundamental triangle
            if (t < 0.25f) v = t * 4.0f;
            else if (t < 0.75f) v = 1.0f - (t - 0.25f) * 4.0f;
            else v = (t - 0.75f) * 4.0f - 1.0f;
            // 2nd harmonic (octave up)
            float t2 = t * 2.0f;
            t2 -= (int)t2; // wrap
            float v2;
            if (t2 < 0.25f) v2 = t2 * 4.0f;
            else if (t2 < 0.75f) v2 = 1.0f - (t2 - 0.25f) * 4.0f;
            else v2 = (t2 - 0.75f) * 4.0f - 1.0f;
            wt_organ[i] = (int16_t)((v * 0.7f + v2 * 0.3f) * 32767.0f);
        }

        // 7: Phaser (detuned triangle pair)
        {
            float v1, v2;
            if (t < 0.25f) v1 = t * 4.0f;
            else if (t < 0.75f) v1 = 1.0f - (t - 0.25f) * 4.0f;
            else v1 = (t - 0.75f) * 4.0f - 1.0f;
            // Slightly detuned (7/8 freq ratio in phase space)
            float t2 = t * 7.0f / 8.0f;
            t2 -= (int)t2;
            if (t2 < 0.25f) v2 = t2 * 4.0f;
            else if (t2 < 0.75f) v2 = 1.0f - (t2 - 0.25f) * 4.0f;
            else v2 = (t2 - 0.75f) * 4.0f - 1.0f;
            wt_phaser[i] = (int16_t)((v1 * 0.5f + v2 * 0.5f) * 32767.0f);
        }
    }
}

// ============================================================
// SFX Channel State
// ============================================================

typedef struct {
    int sfx_num;            // -1 = inactive
    int note_idx;           // current note index (0-31)
    int note_end;           // last note + 1 (exclusive)
    int tick;               // sample counter within current note
    int samples_per_note;   // speed * SAMPLES_PER_TICK

    // Loop range from SFX header
    int loop_start;
    int loop_end;           // 0 = no loop

    // Current note decoded
    p8_note_t cur_note;
    p8_note_t next_note;    // for slide effect

    // Synthesis state
    uint32_t phase;         // phase accumulator
    uint32_t phase_inc;     // current frequency as phase increment
    float    cur_vol;       // current volume (0.0-1.0), for fade effects
    float    target_vol;    // target volume for this note

    // Noise LFSR
    uint16_t noise_lfsr;
    uint8_t  noise_last_idx; // last phase index for noise clock

    // Effect state (precomputed in start_note to avoid powf/sinf in ISR)
    uint32_t slide_inc_start;  // phase_inc at start of slide
    uint32_t slide_inc_end;    // phase_inc at end of slide (next note)
    float    slide_vol_start;  // volume at start of slide
    float    slide_vol_end;    // volume at end of slide (next note)
    uint32_t vibrato_counter;  // vibrato LFO counter (samples)
    uint32_t vibrato_inc_lo;   // phase_inc at -0.5 semitone
    uint32_t vibrato_inc_hi;   // phase_inc at +0.5 semitone

    // Music ownership
    bool     music_owned;    // true if this channel is playing for the music sequencer
} sfx_ch_t;

#define NUM_CHANNELS 4
static sfx_ch_t sfx_channels[NUM_CHANNELS];

// ============================================================
// Music Sequencer State
// ============================================================

typedef struct {
    int  current_pattern;    // -1 = not playing
    int  channel_mask;       // bitmask of channels music may use (0xF = all)
    bool playing;
    // Fade state (ISR-safe: integer arithmetic only)
    int  fade_samples_total; // total samples in fade (0 = no fade)
    int  fade_samples_left;  // remaining samples (counts down)
} music_state_t;

static music_state_t music;

// ============================================================
// Waveform sample (phase → sample)
// ============================================================

static inline int16_t __not_in_flash_func(wave_sample)(uint8_t waveform, uint32_t phase,
                                                         uint16_t *noise_lfsr, uint8_t *noise_last) {
    uint8_t idx = phase >> 24;  // top 8 bits

    switch (waveform) {
    case 0: return wt_triangle[idx];
    case 1: return wt_tilted_saw[idx];
    case 2: return wt_saw[idx];
    case 3: return wt_square[idx];
    case 4: return wt_pulse[idx];
    case 5: return wt_organ[idx];
    case 6: {
        // Noise: clock LFSR when phase wraps around (top 8 bits change)
        if (idx != *noise_last) {
            uint16_t lfsr = *noise_lfsr;
            uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 1) ^ (lfsr >> 5) ^ (lfsr >> 6)) & 1;
            *noise_lfsr = (lfsr >> 1) | (bit << 15);
            *noise_last = idx;
        }
        return (int16_t)(*noise_lfsr - 32768);
    }
    case 7: return wt_phaser[idx];
    default: return 0;
    }
}

// ============================================================
// Note Transition
// ============================================================

static void __not_in_flash_func(start_note)(sfx_ch_t *ch, const uint8_t *mem) {
    ch->cur_note = decode_note(mem, ch->sfx_num, ch->note_idx);
    ch->target_vol = (float)ch->cur_note.volume / 7.0f;
    ch->tick = 0;

    // Pre-decode next note for slide effect
    if (ch->note_idx + 1 < ch->note_end) {
        ch->next_note = decode_note(mem, ch->sfx_num, ch->note_idx + 1);
    } else {
        ch->next_note = ch->cur_note;
    }

    // Base phase increment for this note (all from precomputed RAM tables)
    int p = ch->cur_note.pitch;
    if (p < 0) p = 0;
    if (p > 63) p = 63;
    ch->phase_inc = pitch_to_phase_inc[p];

    // Effect-specific init (all from precomputed RAM tables — no powf/sinf)
    switch (ch->cur_note.effect) {
    case 1: { // Slide — glide from current freq to next note's freq
        ch->slide_inc_start = ch->phase_inc;
        int np = ch->next_note.pitch;
        if (np < 0) np = 0;
        if (np > 63) np = 63;
        ch->slide_inc_end = pitch_to_phase_inc[np];
        ch->slide_vol_start = ch->target_vol;
        ch->slide_vol_end = (float)ch->next_note.volume / 7.0f;
        break;
    }
    case 2: { // Vibrato — use precomputed ±0.5 semitone tables
        ch->vibrato_counter = 0;
        ch->vibrato_inc_lo = pitch_phase_inc_lo[p];
        ch->vibrato_inc_hi = pitch_phase_inc_hi[p];
        break;
    }
    case 4: // Fade in — volume ramps from 0 to target
        ch->cur_vol = 0.0f;
        break;
    case 5: // Fade out — volume ramps from target to 0
        ch->cur_vol = ch->target_vol;
        break;
    default:
        ch->cur_vol = ch->target_vol;
        break;
    }

    // Volume 0 with no effect = silence (don't reset phase)
    if (ch->cur_note.volume == 0 && ch->cur_note.effect == 0) {
        ch->cur_vol = 0.0f;
    }
}

// ============================================================
// Per-Sample Effect Processing
// ============================================================

static inline void __not_in_flash_func(apply_effect)(sfx_ch_t *ch) {
    // progress: 0.0 at note start, 1.0 at note end (fixed-point: tick/samples_per_note)
    int spn = ch->samples_per_note;

    switch (ch->cur_note.effect) {
    case 0: // None
        break;
    case 1: { // Slide — linear interpolation of phase_inc and volume
        // Lerp phase_inc: start + (end - start) * tick / spn
        int32_t diff = (int32_t)ch->slide_inc_end - (int32_t)ch->slide_inc_start;
        ch->phase_inc = (uint32_t)((int32_t)ch->slide_inc_start +
            (int32_t)(((int64_t)diff * ch->tick) / spn));
        // Lerp volume
        float vdiff = ch->slide_vol_end - ch->slide_vol_start;
        ch->cur_vol = ch->slide_vol_start + vdiff * (float)ch->tick / (float)spn;
        break;
    }
    case 2: { // Vibrato — triangle LFO between precomputed lo/hi phase_inc
        ch->vibrato_counter++;
        // LFO period = SAMPLES_PER_TICK (~172 samples = one full cycle)
        uint32_t lfo_pos = ch->vibrato_counter % SAMPLES_PER_TICK;
        uint32_t half = SAMPLES_PER_TICK / 2;
        // Triangle LFO: 0→1→0 over one period
        uint32_t t;
        if (lfo_pos < half)
            t = (lfo_pos * 256) / half;        // 0..255
        else
            t = ((SAMPLES_PER_TICK - lfo_pos) * 256) / half; // 255..0
        // Lerp between lo and hi
        int32_t lo = (int32_t)ch->vibrato_inc_lo;
        int32_t hi = (int32_t)ch->vibrato_inc_hi;
        ch->phase_inc = (uint32_t)(lo + ((hi - lo) * (int32_t)t) / 256);
        ch->cur_vol = ch->target_vol;
        break;
    }
    case 3: { // Drop — phase_inc drops linearly to zero
        if (spn > 0) {
            uint64_t inc = (uint64_t)pitch_to_phase_inc[ch->cur_note.pitch & 63];
            ch->phase_inc = (uint32_t)(inc * (uint32_t)(spn - ch->tick) / (uint32_t)spn);
        }
        ch->cur_vol = ch->target_vol;
        break;
    }
    case 4: // Fade in
        if (spn > 0)
            ch->cur_vol = ch->target_vol * (float)ch->tick / (float)spn;
        break;
    case 5: // Fade out
        if (spn > 0)
            ch->cur_vol = ch->target_vol * (float)(spn - ch->tick) / (float)spn;
        break;
    case 6: { // Arpeggio fast (cycle 0, +4, +7 semitones every tick)
        static const int arp_offsets[3] = {0, 4, 7};
        int arp_step = (ch->tick / SAMPLES_PER_TICK) % 3;
        int p = (ch->cur_note.pitch + arp_offsets[arp_step]) & 63;
        ch->phase_inc = pitch_to_phase_inc[p];
        ch->cur_vol = ch->target_vol;
        break;
    }
    case 7: { // Arpeggio slow (cycle 0, +4, +7 every 2 ticks)
        static const int arp_offsets[3] = {0, 4, 7};
        int arp_step = (ch->tick / (SAMPLES_PER_TICK * 2)) % 3;
        int p = (ch->cur_note.pitch + arp_offsets[arp_step]) & 63;
        ch->phase_inc = pitch_to_phase_inc[p];
        ch->cur_vol = ch->target_vol;
        break;
    }
    }
}

// ============================================================
// Music Sequencer — advance to next pattern
// ============================================================

static void music_start_pattern(int pat);

static void __not_in_flash_func(music_advance)(void) {
    if (!music.playing || music.current_pattern < 0) return;

    const uint8_t *mem = p8_mem_cached;
    if (!mem) return;

    int pat = music.current_pattern;
    int addr = P8_MUSIC_BASE + pat * 4;

    // Check flags on current pattern
    bool has_stop = (mem[addr + 2] & 0x80) != 0;
    bool has_loop_end = (mem[addr + 1] & 0x80) != 0;

    if (has_stop) {
        // Stop music
        music.playing = false;
        music.current_pattern = -1;
        // Stop music-owned channels
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (sfx_channels[c].music_owned) {
                sfx_channels[c].sfx_num = -1;
                sfx_channels[c].music_owned = false;
            }
        }
        return;
    }

    if (has_loop_end) {
        // Find the loop start pattern (search backwards for loop_begin flag)
        int loop_target = 0;
        for (int p = pat; p >= 0; p--) {
            int a = P8_MUSIC_BASE + p * 4;
            if (mem[a] & 0x80) { // loop_begin flag on byte 0
                loop_target = p;
                break;
            }
        }
        music_start_pattern(loop_target);
        return;
    }

    // Advance to next pattern
    if (pat + 1 < 64) {
        music_start_pattern(pat + 1);
    } else {
        music.playing = false;
        music.current_pattern = -1;
    }
}

static void __not_in_flash_func(music_start_pattern)(int pat) {
    const uint8_t *mem = p8_mem_cached;
    if (!mem) return;

    music.current_pattern = pat;
    music.playing = true;

    int addr = P8_MUSIC_BASE + pat * 4;

    for (int c = 0; c < NUM_CHANNELS; c++) {
        if (!(music.channel_mask & (1 << c))) continue;

        uint8_t ch_byte = mem[addr + c];
        // bit 6 set = channel disabled (no SFX assigned)
        if (ch_byte & 0x40) {
            if (sfx_channels[c].music_owned) {
                sfx_channels[c].sfx_num = -1;
                sfx_channels[c].music_owned = false;
            }
            continue;
        }

        int sfx_num = ch_byte & 0x3F;
        sfx_ch_t *ch = &sfx_channels[c];
        ch->sfx_num = sfx_num;
        ch->note_idx = 0;
        ch->note_end = P8_SFX_NOTES;
        ch->music_owned = true;

        uint8_t spd = sfx_speed(mem, sfx_num);
        if (spd == 0) spd = 1;
        ch->samples_per_note = spd * SAMPLES_PER_TICK;

        ch->loop_start = sfx_loop_start(mem, sfx_num);
        ch->loop_end = sfx_loop_end(mem, sfx_num);

        ch->phase = 0;
        ch->noise_lfsr = 0xACE1;
        ch->noise_last_idx = 0;

        start_note(ch, mem);
    }
}

// ============================================================
// Per-Sample Channel Processing
// ============================================================

static inline int16_t __not_in_flash_func(channel_sample)(sfx_ch_t *ch) {
    if (ch->sfx_num < 0) return 0;

    const uint8_t *mem = p8_mem_cached;
    if (!mem) return 0;

    // Generate waveform sample
    int16_t raw = wave_sample(ch->cur_note.waveform, ch->phase,
                               &ch->noise_lfsr, &ch->noise_last_idx);

    // Apply volume
    int32_t sample = ((int32_t)raw * (int32_t)(ch->cur_vol * 255.0f)) >> 8;

    // Advance phase
    ch->phase += ch->phase_inc;

    // Advance tick and apply effects
    ch->tick++;
    apply_effect(ch);

    // Check for note transition
    if (ch->tick >= ch->samples_per_note) {
        ch->note_idx++;

        // Handle SFX-internal loop (loop_start to loop_end)
        if (ch->loop_end > 0 && ch->loop_start < ch->loop_end) {
            if (ch->note_idx >= ch->loop_end) {
                ch->note_idx = ch->loop_start;
            }
        }

        if (ch->note_idx >= ch->note_end) {
            // SFX finished
            ch->sfx_num = -1;

            // If music-owned, check if all music channels are done
            if (ch->music_owned) {
                ch->music_owned = false;
                bool all_done = true;
                for (int c = 0; c < NUM_CHANNELS; c++) {
                    if (sfx_channels[c].music_owned && sfx_channels[c].sfx_num >= 0) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done) {
                    music_advance();
                }
            }
            return (int16_t)sample;
        }

        start_note(ch, mem);
    }

    return (int16_t)sample;
}

// ============================================================
// Public API
// ============================================================

void p8_sfx_init(void) {
    build_pitch_table();
    build_wavetables();

    // Cache the memory pointer so ISR never calls flash-resident p8_get_memory()
    p8_mem_cached = p8_get_memory();

    for (int i = 0; i < NUM_CHANNELS; i++) {
        memset(&sfx_channels[i], 0, sizeof(sfx_ch_t));
        sfx_channels[i].sfx_num = -1;
        sfx_channels[i].noise_lfsr = 0xACE1;
    }

    music.current_pattern = -1;
    music.channel_mask = 0xF;
    music.playing = false;
    music.fade_samples_total = 0;
    music.fade_samples_left = 0;
}

void p8_sfx_play(int n, int channel, int offset, int length) {
    const uint8_t *mem = p8_mem_cached;
    if (!mem) return;
    if (n < 0 || n >= P8_SFX_SLOTS) return;

    // Auto-assign channel: find first free (non-music) channel
    if (channel < 0) {
        channel = -1;
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (sfx_channels[c].sfx_num < 0 && !sfx_channels[c].music_owned) {
                channel = c;
                break;
            }
        }
        // If no free channel, steal the first non-music channel
        if (channel < 0) {
            for (int c = 0; c < NUM_CHANNELS; c++) {
                if (!sfx_channels[c].music_owned) {
                    channel = c;
                    break;
                }
            }
        }
        // Last resort: use channel 0
        if (channel < 0) channel = 0;
    }

    if (channel < 0 || channel >= NUM_CHANNELS) return;

    sfx_ch_t *ch = &sfx_channels[channel];
    ch->sfx_num = n;
    ch->note_idx = (offset >= 0 && offset < P8_SFX_NOTES) ? offset : 0;
    ch->note_end = (length > 0 && ch->note_idx + length <= P8_SFX_NOTES)
                   ? ch->note_idx + length : P8_SFX_NOTES;
    ch->music_owned = false;

    uint8_t spd = sfx_speed(mem, n);
    if (spd == 0) spd = 1;
    ch->samples_per_note = spd * SAMPLES_PER_TICK;

    ch->loop_start = sfx_loop_start(mem, n);
    ch->loop_end = sfx_loop_end(mem, n);

    ch->phase = 0;
    ch->noise_lfsr = 0xACE1;
    ch->noise_last_idx = 0;

    start_note(ch, mem);
}

void p8_sfx_stop(int channel) {
    if (channel == -1) {
        // Stop all
        for (int c = 0; c < NUM_CHANNELS; c++) {
            sfx_channels[c].sfx_num = -1;
            sfx_channels[c].music_owned = false;
        }
    } else if (channel == -2) {
        // Stop music-owned channels only
        for (int c = 0; c < NUM_CHANNELS; c++) {
            if (sfx_channels[c].music_owned) {
                sfx_channels[c].sfx_num = -1;
                sfx_channels[c].music_owned = false;
            }
        }
    } else if (channel >= 0 && channel < NUM_CHANNELS) {
        sfx_channels[channel].sfx_num = -1;
        sfx_channels[channel].music_owned = false;
    }
}

void p8_music_play(int n, int fade_len, int channel_mask) {
    if (n < 0 || n >= 64) {
        // music(-1) with fade: fade out then stop
        if (fade_len > 0 && music.playing) {
            music.fade_samples_total = (fade_len * SAMPLE_RATE) / 1000;
            music.fade_samples_left = music.fade_samples_total;
        } else {
            p8_music_stop();
        }
        return;
    }

    music.channel_mask = (channel_mask > 0) ? channel_mask : 0xF;
    music.fade_samples_total = 0;
    music.fade_samples_left = 0;

    // Stop any currently music-owned channels
    for (int c = 0; c < NUM_CHANNELS; c++) {
        if (sfx_channels[c].music_owned) {
            sfx_channels[c].sfx_num = -1;
            sfx_channels[c].music_owned = false;
        }
    }

    music_start_pattern(n);
}

void p8_music_stop(void) {
    music.playing = false;
    music.current_pattern = -1;
    music.fade_samples_total = 0;
    music.fade_samples_left = 0;

    for (int c = 0; c < NUM_CHANNELS; c++) {
        if (sfx_channels[c].music_owned) {
            sfx_channels[c].sfx_num = -1;
            sfx_channels[c].music_owned = false;
        }
    }
}

int16_t __not_in_flash_func(p8_sfx_mix_sample)(void) {
    int32_t mix = 0;
    for (int c = 0; c < NUM_CHANNELS; c++) {
        int32_t s = channel_sample(&sfx_channels[c]);
        // Apply music fade to music-owned channels
        if (sfx_channels[c].music_owned && music.fade_samples_total > 0) {
            s = s * music.fade_samples_left / music.fade_samples_total;
        }
        mix += s;
    }
    // Advance fade counter
    if (music.fade_samples_total > 0 && music.fade_samples_left > 0) {
        music.fade_samples_left--;
        if (music.fade_samples_left <= 0) {
            // Fade complete — stop music
            music.playing = false;
            music.current_pattern = -1;
            music.fade_samples_total = 0;
            for (int c = 0; c < NUM_CHANNELS; c++) {
                if (sfx_channels[c].music_owned) {
                    sfx_channels[c].sfx_num = -1;
                    sfx_channels[c].music_owned = false;
                }
            }
        }
    }
    // Clip
    if (mix > 32767) mix = 32767;
    if (mix < -32768) mix = -32768;
    return (int16_t)mix;
}

int p8_sfx_get_current(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return -1;
    return sfx_channels[channel].sfx_num;
}

int p8_sfx_get_note(int channel) {
    if (channel < 0 || channel >= NUM_CHANNELS) return 0;
    if (sfx_channels[channel].sfx_num < 0) return 0;
    return sfx_channels[channel].note_idx;
}

int p8_music_get_pattern(void) {
    return music.playing ? music.current_pattern : -1;
}

int p8_music_get_count(void) {
    // Not tracked yet — return 0
    return 0;
}
