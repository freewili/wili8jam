/*
 * PICO-8 Cartridge Loader
 *
 * Parses .p8 files, loads data sections into virtual memory,
 * and runs the _init/_update/_draw game loop.
 */

#include "p8_cart.h"
#include "p8_api.h"
#include "p8_sfx.h"
#include "p8_preprocess.h"
#include "p8_png.h"
#include "p8_console.h"
#include "p8_editor.h"
#include "gfx.h"
#include "input.h"
#include "audio.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tlsf/tlsf.h"
#include "fatfs/ff.h"
#include "pico/stdlib.h"

#include <string.h>
#include <stdio.h>

static tlsf_t cart_tlsf;

// Current cart path (for reset/run)
static char current_cart_path[256];

// ============================================================
// Hex parsing helpers
// ============================================================

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ============================================================
// Section parsers
// ============================================================

// Parse __gfx__ section: 128 lines of 128 hex nibbles each.
// Each nibble is a pixel color (0-F). Stored in vram as 2 pixels per byte.
// PICO-8 .p8 format: each line has 128 hex chars = 128 pixels = 64 bytes.
// In memory: low nibble is left pixel, high nibble is right pixel.
static void parse_gfx(const char *data, size_t len) {
    uint8_t *mem = p8_get_memory();
    if (!mem) return;

    int row = 0;
    const char *p = data;
    const char *end = data + len;

    while (p < end && row < 128) {
        // Skip whitespace/newlines to find start of row
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        // Parse 128 hex nibbles per row
        int col = 0;
        while (p < end && *p != '\n' && *p != '\r' && col < 128) {
            int v = hex_val(*p);
            if (v < 0) { p++; continue; }
            // PICO-8 .p8 gfx: nibbles are stored left-to-right
            // In vram: byte = (right_pixel << 4) | left_pixel
            // But .p8 format stores them as individual nibbles left-to-right
            int addr = row * 64 + col / 2;
            if (col & 1)
                mem[addr] = (mem[addr] & 0x0F) | (v << 4);
            else
                mem[addr] = (mem[addr] & 0xF0) | v;
            col++;
            p++;
        }
        // Skip rest of line
        while (p < end && *p != '\n') p++;
        row++;
    }
}

// Parse __gff__ section: sprite flags (256 sprites, 2 hex chars each per line).
// Format: 128 hex pairs per line, 2 lines (or 1 long block of 256 hex pairs).
static void parse_gff(const char *data, size_t len) {
    uint8_t *mem = p8_get_memory();
    if (!mem) return;

    const char *p = data;
    const char *end = data + len;
    int idx = 0;

    while (p < end && idx < 256) {
        while (p < end && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
        if (p + 1 >= end) break;
        int hi = hex_val(*p);
        int lo = hex_val(*(p + 1));
        if (hi < 0 || lo < 0) { p++; continue; }
        mem[0x3000 + idx] = (uint8_t)((hi << 4) | lo);
        idx++;
        p += 2;
    }
}

// Parse __map__ section: 128x32 tile map, each cell is 2 hex chars.
// 256 hex chars per line (128 tiles), 32 lines.
static void parse_map(const char *data, size_t len) {
    uint8_t *mem = p8_get_memory();
    if (!mem) return;

    const char *p = data;
    const char *end = data + len;
    int row = 0;

    while (p < end && row < 32) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        int col = 0;
        while (p + 1 < end && *p != '\n' && *p != '\r' && col < 128) {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi < 0 || lo < 0) { p++; continue; }
            // Map lives at 0x2000 in vram
            mem[0x2000 + row * 128 + col] = (uint8_t)((hi << 4) | lo);
            col++;
            p += 2;
        }
        while (p < end && *p != '\n') p++;
        row++;
    }
}

// Parse __sfx__ section: 64 sound effects, each line = 168 hex chars.
// .p8 text format: EE SS LL NN + 32 notes × 5 hex chars (PPWVE)
// Memory format at 0x3200 (68 bytes per SFX):
//   bytes 0-63: 32 notes as 16-bit LE words
//     bit 0: custom, bits 1-6: pitch, bits 7-9: waveform, bits 10-12: vol, bits 13-15: effect
//   byte 64: editor mode, byte 65: speed, byte 66: loop start, byte 67: loop end
static void parse_sfx(const char *data, size_t len) {
    uint8_t *mem = p8_get_memory();
    if (!mem) return;

    const char *p = data;
    const char *end = data + len;
    int sfx_idx = 0;

    while (p < end && sfx_idx < 64) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        int addr = 0x3200 + sfx_idx * 68;

        // Read 4 header bytes as hex pairs: editor, speed, loop_start, loop_end
        uint8_t header[4] = {0, 0, 0, 0};
        for (int h = 0; h < 4 && p + 1 < end && *p != '\n'; h++) {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) header[h] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        // Store header at bytes 64-67 in memory
        mem[addr + 64] = header[0]; // editor mode
        mem[addr + 65] = header[1]; // speed
        mem[addr + 66] = header[2]; // loop start
        mem[addr + 67] = header[3]; // loop end

        // Read 32 notes, each 5 hex chars: PP W V E
        for (int n = 0; n < 32 && p < end && *p != '\n' && *p != '\r'; n++) {
            // Need 5 hex chars
            if (p + 4 >= end) break;
            int pp_hi = hex_val(p[0]);
            int pp_lo = hex_val(p[1]);
            int w_nib = hex_val(p[2]);
            int v_nib = hex_val(p[3]);
            int e_nib = hex_val(p[4]);
            p += 5;

            if (pp_hi < 0 || pp_lo < 0 || w_nib < 0 || v_nib < 0 || e_nib < 0)
                continue;

            int pitch    = ((pp_hi << 4) | pp_lo) & 0x3F;
            int custom   = (w_nib >> 3) & 1;
            int waveform = w_nib & 7;
            int volume   = v_nib & 7;
            int effect   = e_nib & 7;

            // Pack into 16-bit LE word
            uint16_t word = (uint16_t)(
                (custom << 0) |
                (pitch << 1) |
                (waveform << 7) |
                (volume << 10) |
                (effect << 13)
            );
            mem[addr + n * 2]     = (uint8_t)(word & 0xFF);
            mem[addr + n * 2 + 1] = (uint8_t)(word >> 8);
        }

        while (p < end && *p != '\n') p++;
        sfx_idx++;
    }
}

// Parse __music__ section: 64 music patterns.
// .p8 text format: "FF AABBCCDD" per line
//   FF = flags byte (bit 0=loop_begin, bit 1=loop_end, bit 2=stop)
//   AA-DD = 4 channel SFX assignments (bit 6 set = channel disabled)
// Memory format at 0x3100 (4 bytes per pattern):
//   byte 0: ch0 sfx# (bits 0-5) + bit 6 (disabled) + bit 7 (loop_begin from flags bit 0)
//   byte 1: ch1 sfx# + bit 6 + bit 7 (loop_end from flags bit 1)
//   byte 2: ch2 sfx# + bit 6 + bit 7 (stop from flags bit 2)
//   byte 3: ch3 sfx# + bit 6
static void parse_music(const char *data, size_t len) {
    uint8_t *mem = p8_get_memory();
    if (!mem) return;

    const char *p = data;
    const char *end = data + len;
    int pat_idx = 0;

    while (p < end && pat_idx < 64) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        // Read flags byte (2 hex chars)
        while (p < end && *p == ' ') p++;
        uint8_t flags = 0;
        if (p + 1 < end && *p != '\n') {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) flags = (uint8_t)((hi << 4) | lo);
            p += 2;
        }

        // Skip space
        while (p < end && *p == ' ') p++;

        // Read 4 channel bytes (8 hex chars)
        uint8_t ch_bytes[4] = {0x41, 0x41, 0x41, 0x41}; // default: disabled
        for (int c = 0; c < 4 && p + 1 < end && *p != '\n' && *p != '\r'; c++) {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) ch_bytes[c] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }

        // Merge flags into channel bytes (bit 7)
        // flags bit 0 → byte 0 bit 7 (loop_begin)
        // flags bit 1 → byte 1 bit 7 (loop_end)
        // flags bit 2 → byte 2 bit 7 (stop)
        if (flags & 0x01) ch_bytes[0] |= 0x80;
        if (flags & 0x02) ch_bytes[1] |= 0x80;
        if (flags & 0x04) ch_bytes[2] |= 0x80;

        int addr = 0x3100 + pat_idx * 4;
        mem[addr + 0] = ch_bytes[0];
        mem[addr + 1] = ch_bytes[1];
        mem[addr + 2] = ch_bytes[2];
        mem[addr + 3] = ch_bytes[3];

        while (p < end && *p != '\n') p++;
        pat_idx++;
    }
}

// ============================================================
// .p8 file parser
// ============================================================

// Find the start of a section by its header (e.g., "__lua__\n").
// Returns pointer to the line AFTER the header, or NULL if not found.
static const char *find_section(const char *data, size_t len, const char *header) {
    size_t hlen = strlen(header);
    const char *p = data;
    const char *end = data + len;

    while (p < end) {
        // Check if current position matches header at start of line
        if ((size_t)(end - p) >= hlen && memcmp(p, header, hlen) == 0) {
            p += hlen;
            // Skip rest of header line
            while (p < end && *p != '\n') p++;
            if (p < end) p++; // skip newline
            return p;
        }
        // Skip to next line
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return NULL;
}

// Find the end of a section (next __xxx__ header or end of data).
static const char *find_section_end(const char *start, const char *data_end) {
    const char *p = start;
    while (p < data_end) {
        if (*p == '_' && p + 1 < data_end && *(p + 1) == '_') {
            // Check if this looks like a section header (at start of line)
            if (p == start || *(p - 1) == '\n')
                return p;
        }
        p++;
    }
    return data_end;
}

char *p8_cart_parse(const char *data, size_t data_len, size_t *lua_len) {
    uint8_t *mem = p8_get_memory();
    if (mem) memset(mem, 0, 0x8000); // Clear virtual memory

    const char *data_end = data + data_len;
    char *lua_code = NULL;
    *lua_len = 0;

    // Parse __gfx__
    const char *sec = find_section(data, data_len, "__gfx__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        parse_gfx(sec, sec_end - sec);
    }

    // Parse __gff__
    sec = find_section(data, data_len, "__gff__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        parse_gff(sec, sec_end - sec);
    }

    // Parse __map__
    sec = find_section(data, data_len, "__map__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        parse_map(sec, sec_end - sec);
    }

    // Parse __sfx__
    sec = find_section(data, data_len, "__sfx__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        parse_sfx(sec, sec_end - sec);
    }

    // Parse __music__
    sec = find_section(data, data_len, "__music__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        parse_music(sec, sec_end - sec);
    }

    // Extract __lua__ section — may span multiple tabs (-->8)
    sec = find_section(data, data_len, "__lua__");
    if (sec) {
        const char *sec_end = find_section_end(sec, data_end);
        size_t sec_len = sec_end - sec;

        // Allocate and copy lua code
        lua_code = (char *)tlsf_malloc(cart_tlsf, sec_len + 1);
        if (lua_code) {
            memcpy(lua_code, sec, sec_len);
            lua_code[sec_len] = '\0';
            *lua_len = sec_len;
        }
    }

    return lua_code;
}

// ============================================================
// Game loop
// ============================================================

// Check if a global function exists
static bool has_global_func(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    bool exists = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return exists;
}

// Call a global function, return true on success
static bool call_global(lua_State *L, const char *name) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return true; // Not defined = ok, just skip
    }
    int status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) {
            printf("ERROR: %s\n", err);
            p8_console_printf("error: %s\n", err);
        }
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Internal game loop — shared by gameloop (with _init) and resume (without _init)
static void cart_gameloop_inner(lua_State *L) {
    // Determine frame rate: 60fps if _update60 exists, else 30fps
    bool use_60fps = has_global_func(L, "_update60");
    const char *update_fn = use_60fps ? "_update60" : "_update";
    uint32_t frame_us = use_60fps ? 16667 : 33333;
    p8_set_target_fps(use_60fps ? 60 : 30);

    printf("Game loop: %s @ %dfps\n", update_fn, use_60fps ? 60 : 30);

    // Tell p8_api that flip() shouldn't sleep (game loop handles timing)
    p8_set_gameloop_mode(true);

    // Use PICO-8 screen-drawing print during game loop (not console_print)
    p8_register_print(L);

    // Main game loop
    while (true) {
        absolute_time_t frame_start = get_absolute_time();

        // Update input state
        input_update();

        // Check for ESC key to break out of game loop
        if (input_key(0x29)) { // HID_KEY_ESCAPE = 0x29
            printf("ESC pressed, stopping cart\n");
            break;
        }

        // Call _update or _update60
        if (!call_global(L, update_fn))
            break;

        // Call _draw (auto-flips after _draw per PICO-8 behavior)
        if (has_global_func(L, "_draw")) {
            if (!call_global(L, "_draw"))
                break;

            // Measure CPU usage before flip (game logic only, not DVI blit)
            int64_t game_elapsed = absolute_time_diff_us(frame_start, get_absolute_time());
            p8_set_cpu_usage((float)game_elapsed / (float)frame_us);

            gfx_flip();
        } else {
            int64_t game_elapsed = absolute_time_diff_us(frame_start, get_absolute_time());
            p8_set_cpu_usage((float)game_elapsed / (float)frame_us);
        }

        // Frame timing — sleep for remaining time (includes flip cost)
        int64_t total_elapsed = absolute_time_diff_us(frame_start, get_absolute_time());
        int64_t remaining = (int64_t)frame_us - total_elapsed;
        if (remaining > 0)
            sleep_us(remaining);
    }

    p8_set_gameloop_mode(false);

    // Stop all audio (SFX + music) when exiting game loop
    p8_sfx_stop(-1);
    p8_music_stop();

    // Reset draw state (palette, camera, clip, etc.)
    p8_reset_draw_state();

    // Flush buffered keyboard input so REPL doesn't get game keypresses
    input_flush();

    // Restore console print for REPL
    p8_console_register(L);
}

void p8_cart_gameloop(lua_State *L) {
    // Call _init() once, then run the game loop
    p8_set_gameloop_mode(true);
    p8_register_print(L);
    if (!call_global(L, "_init")) {
        printf("Cart _init() failed, entering REPL\n");
        p8_set_gameloop_mode(false);
        return;
    }
    cart_gameloop_inner(L);
}

void p8_cart_gameloop_resume(lua_State *L) {
    // Resume game loop without calling _init()
    cart_gameloop_inner(L);
}

// ============================================================
// Cart loading
// ============================================================

// Read a file into PSRAM buffer (same as main.cpp's helper)
static char *cart_read_file(const char *path, size_t *out_size) {
    static FIL fil;  // static to avoid ~600 bytes on stack
    FRESULT fres = f_open(&fil, path, FA_READ);
    if (fres != FR_OK) {
        printf("  f_open failed: %d\n", fres);
        return NULL;
    }

    FSIZE_t fsize = f_size(&fil);
    if (fsize == 0) {
        printf("  file is empty (0 bytes)\n");
        f_close(&fil);
        return NULL;
    }

    char *buf = (char *)tlsf_malloc(cart_tlsf, (size_t)fsize + 1);
    if (!buf) {
        printf("  malloc failed for %lu bytes\n", (unsigned long)fsize);
        f_close(&fil);
        return NULL;
    }

    UINT br;
    FRESULT res = f_read(&fil, buf, (UINT)fsize, &br);
    f_close(&fil);

    if (res != FR_OK || br != (UINT)fsize) {
        printf("  f_read failed: res=%d, got %u/%lu bytes\n",
               res, (unsigned)br, (unsigned long)fsize);
        tlsf_free(cart_tlsf, buf);
        return NULL;
    }

    buf[br] = '\0';
    *out_size = (size_t)br;
    return buf;
}

static bool is_p8png(const char *path) {
    size_t len = strlen(path);
    return (len >= 7 && strcmp(path + len - 7, ".p8.png") == 0);
}

int p8_cart_load(lua_State *L, const char *path) {
    (void)L;
    printf("Loading cart: %s\n", path);

    // Save current cart path
    snprintf(current_cart_path, sizeof(current_cart_path), "%s", path);

    // Read file
    audio_pause();
    size_t file_len;
    char *file_data = cart_read_file(path, &file_len);
    audio_resume();
    if (!file_data) {
        printf("ERROR: cannot read %s\n", path);
        p8_console_printf("error: cannot read %s\n", path);
        p8_console_draw(); gfx_flip();
        return -1;
    }

    size_t lua_len;
    char *lua_code;

    if (is_p8png(path)) {
        lua_code = p8_png_load((const uint8_t *)file_data, file_len, &lua_len);
        tlsf_free(cart_tlsf, file_data);
    } else {
        lua_code = p8_cart_parse(file_data, file_len, &lua_len);
        tlsf_free(cart_tlsf, file_data);
    }

    if (!lua_code) {
        printf("ERROR: no code in %s\n", path);
        p8_console_printf("error: no code in %s\n", path);
        p8_console_draw(); gfx_flip();
        return -1;
    }

    // Free lua code — we only loaded data sections
    tlsf_free(cart_tlsf, lua_code);

    // Load code into editor buffer (.p8 only — .p8.png code is compressed)
    if (!is_p8png(path))
        p8_editor_load(path);

    printf("Loaded: %s\n", path);
    p8_console_printf("loaded %s\n", path);
    p8_console_draw(); gfx_flip();

    return 0;
}

int p8_cart_run(lua_State *L, const char *path) {
    printf("Loading cart: %s\n", path);

    // Save current cart path for reset/run
    snprintf(current_cart_path, sizeof(current_cart_path), "%s", path);

    // Clear all user-defined globals from any previously loaded cart
    p8_cleanup_globals(L);
    p8_reset_draw_state();

    // Clear all PICO-8 virtual memory (sprites, map, flags, sfx, screen)
    uint8_t *mem = p8_get_memory();
    if (mem) memset(mem, 0, 0x8000);

    // Read file
    audio_pause();
    size_t file_len;
    char *file_data = cart_read_file(path, &file_len);
    audio_resume();
    if (!file_data) {
        printf("ERROR: cannot read %s\n", path);
        p8_console_printf("error: cannot read %s\n", path);
        p8_console_draw(); gfx_flip();
        return -1;
    }

    size_t lua_len;
    char *lua_code;

    if (is_p8png(path)) {
        // .p8.png: PNG decode + steganography + code decompression
        // p8_png_load loads assets directly into virtual memory
        lua_code = p8_png_load((const uint8_t *)file_data, file_len, &lua_len);
        tlsf_free(cart_tlsf, file_data);
    } else {
        // .p8 text format
        lua_code = p8_cart_parse(file_data, file_len, &lua_len);
        tlsf_free(cart_tlsf, file_data);
    }

    if (!lua_code) {
        printf("ERROR: no code in %s\n", path);
        p8_console_printf("error: no code in %s\n", path);
        p8_console_draw(); gfx_flip();
        return -1;
    }

    // Preprocess PICO-8 syntax
    size_t pp_len;
    char *pp_code = p8_preprocess(lua_code, lua_len, &pp_len);

    const char *code;
    size_t code_len;
    if (pp_code) {
        tlsf_free(cart_tlsf, lua_code);
        code = pp_code;
        code_len = pp_len;
    } else {
        // Preprocessing failed or returned NULL — use original
        code = lua_code;
        code_len = lua_len;
    }

    // Load and execute the lua code (defines _init/_update/_draw)
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);

    int status = luaL_loadbuffer(L, code, code_len, chunkname);
    if (pp_code) tlsf_free(cart_tlsf, pp_code);
    else tlsf_free(cart_tlsf, lua_code);

    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) {
            printf("ERROR: %s\n", err);
            p8_console_printf("error: %s\n", err);
            p8_console_draw(); gfx_flip();
        }
        lua_pop(L, 1);
        return -1;
    }

    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) {
            printf("ERROR: %s\n", err);
            p8_console_printf("error: %s\n", err);
            p8_console_draw(); gfx_flip();
        }
        lua_pop(L, 1);
        return -1;
    }

    // Enter game loop if _update, _update60, or _draw is defined
    if (has_global_func(L, "_update") || has_global_func(L, "_update60") || has_global_func(L, "_draw")) {
        p8_cart_gameloop(L);
    }

    return 0;
}

// ============================================================
// reload() — re-read cart data from SD
// ============================================================

int p8_cart_reload(int dest, int src, int len, const char *filename) {
    const char *path = (filename && *filename) ? filename : current_cart_path;
    if (!path || !*path) return -1;

    uint8_t *mem = p8_get_memory();
    if (!mem) return -1;

    // Clamp to valid virtual memory range (0x0000-0x7FFF)
    if (dest < 0) dest = 0;
    if (src < 0) src = 0;
    if (len <= 0) return 0;
    if (dest + len > 0x8000) len = 0x8000 - dest;
    if (src + len > 0x8000) len = 0x8000 - src;
    if (len <= 0) return 0;

    // Pause audio during SD access (PSRAM/SPI bus contention)
    audio_pause();

    // Read the .p8 file from SD
    size_t file_len;
    char *file_data = cart_read_file(path, &file_len);
    if (!file_data) {
        audio_resume();
        return -1;
    }

    // Save current virtual memory
    uint8_t *saved = (uint8_t *)tlsf_malloc(cart_tlsf, 0x8000);
    if (!saved) {
        tlsf_free(cart_tlsf, file_data);
        audio_resume();
        return -1;
    }
    memcpy(saved, mem, 0x8000);

    // Parse .p8 into virtual memory (gives us fresh cart data)
    size_t lua_len;
    char *lua_code = p8_cart_parse(file_data, file_len, &lua_len);
    tlsf_free(cart_tlsf, file_data);
    if (lua_code) tlsf_free(cart_tlsf, lua_code); // don't need lua code

    // Copy the requested range from fresh data to saved buffer
    memcpy(saved + dest, mem + src, len);

    // Restore everything (with the reloaded range applied)
    memcpy(mem, saved, 0x8000);
    tlsf_free(cart_tlsf, saved);

    audio_resume();
    return 0;
}

// ============================================================
// cstore() — write virtual memory back to cart file on SD
// ============================================================

static const char hex_chars[] = "0123456789abcdef";

// Serialize __gfx__ section from virtual memory (0x0000-0x1FFF).
// 128 rows of 128 hex nibbles each + newline = 129 chars per row.
// Total: 128 * 129 = 16512 chars.
static size_t serialize_gfx(char *out, const uint8_t *mem) {
    char *p = out;
    for (int row = 0; row < 128; row++) {
        for (int col = 0; col < 128; col++) {
            int addr = row * 64 + col / 2;
            int nib = (col & 1) ? ((mem[addr] >> 4) & 0xF) : (mem[addr] & 0xF);
            *p++ = hex_chars[nib];
        }
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

// Serialize __gff__ section (0x3000-0x30FF): 2 lines of 128 hex pairs.
static size_t serialize_gff(char *out, const uint8_t *mem) {
    char *p = out;
    for (int line = 0; line < 2; line++) {
        for (int i = 0; i < 128; i++) {
            uint8_t v = mem[0x3000 + line * 128 + i];
            *p++ = hex_chars[(v >> 4) & 0xF];
            *p++ = hex_chars[v & 0xF];
        }
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

// Serialize __map__ section (0x2000-0x2FFF): 32 lines of 256 hex chars.
static size_t serialize_map(char *out, const uint8_t *mem) {
    char *p = out;
    for (int row = 0; row < 32; row++) {
        for (int col = 0; col < 128; col++) {
            uint8_t v = mem[0x2000 + row * 128 + col];
            *p++ = hex_chars[(v >> 4) & 0xF];
            *p++ = hex_chars[v & 0xF];
        }
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

// Serialize __sfx__ section (0x3200-0x42FF): 64 lines of 168 hex chars.
// Format: EE SS LL NN + 32 notes × 5 hex chars (PPWVE)
static size_t serialize_sfx(char *out, const uint8_t *mem) {
    char *p = out;
    for (int sfx = 0; sfx < 64; sfx++) {
        int addr = 0x3200 + sfx * 68;
        // Header: editor mode, speed, loop start, loop end (bytes 64-67)
        for (int h = 0; h < 4; h++) {
            uint8_t v = mem[addr + 64 + h];
            *p++ = hex_chars[(v >> 4) & 0xF];
            *p++ = hex_chars[v & 0xF];
        }
        // 32 notes: decode 16-bit LE → 5 hex chars PPWVE
        for (int n = 0; n < 32; n++) {
            uint16_t word = (uint16_t)(mem[addr + n * 2] | (mem[addr + n * 2 + 1] << 8));
            int pitch    = (word >> 1) & 0x3F;
            int custom   = word & 1;
            int waveform = (word >> 7) & 7;
            int volume   = (word >> 10) & 7;
            int effect   = (word >> 13) & 7;
            *p++ = hex_chars[(pitch >> 4) & 0xF];
            *p++ = hex_chars[pitch & 0xF];
            *p++ = hex_chars[(custom << 3) | waveform];
            *p++ = hex_chars[volume];
            *p++ = hex_chars[effect];
        }
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

// Serialize __music__ section (0x3100-0x31FF): 64 lines of "FF AABBCCDD".
// Memory: 4 bytes/pattern, flags in bit 7 of ch bytes 0-2.
static size_t serialize_music(char *out, const uint8_t *mem) {
    char *p = out;
    for (int pat = 0; pat < 64; pat++) {
        int addr = 0x3100 + pat * 4;
        uint8_t ch[4];
        for (int c = 0; c < 4; c++) ch[c] = mem[addr + c];

        // Extract flags from bit 7 of channels 0-2
        uint8_t flags = 0;
        if (ch[0] & 0x80) flags |= 0x01;
        if (ch[1] & 0x80) flags |= 0x02;
        if (ch[2] & 0x80) flags |= 0x04;

        // Clear flag bits from channel bytes for output
        ch[0] &= 0x7F;
        ch[1] &= 0x7F;
        ch[2] &= 0x7F;

        *p++ = hex_chars[(flags >> 4) & 0xF];
        *p++ = hex_chars[flags & 0xF];
        *p++ = ' ';
        for (int c = 0; c < 4; c++) {
            *p++ = hex_chars[(ch[c] >> 4) & 0xF];
            *p++ = hex_chars[ch[c] & 0xF];
        }
        *p++ = '\n';
    }
    return (size_t)(p - out);
}

int p8_cart_cstore(int dest, int src, int len, const char *filename) {
    const char *path = (filename && *filename) ? filename : current_cart_path;
    if (!path || !*path) return -1;

    uint8_t *mem = p8_get_memory();
    if (!mem) return -1;

    // Clamp to valid virtual memory range
    if (dest < 0) dest = 0;
    if (src < 0) src = 0;
    if (len <= 0) return 0;
    if (dest + len > 0x8000) len = 0x8000 - dest;
    if (src + len > 0x8000) len = 0x8000 - src;
    if (len <= 0) return 0;

    audio_pause();

    // Read the original .p8 file to get the __lua__ section and header
    size_t file_len;
    char *file_data = cart_read_file(path, &file_len);

    // Save current virtual memory
    uint8_t *saved = (uint8_t *)tlsf_malloc(cart_tlsf, 0x8000);
    if (!saved) {
        if (file_data) tlsf_free(cart_tlsf, file_data);
        audio_resume();
        return -1;
    }
    memcpy(saved, mem, 0x8000);

    // Extract __lua__ section from existing file (if any)
    char *lua_section = NULL;
    size_t lua_section_len = 0;
    const char *header_end = NULL;

    if (file_data) {
        // Parse .p8 into VM to get the original cart data
        size_t lua_len;
        char *lua_code = p8_cart_parse(file_data, file_len, &lua_len);

        // Copy the requested range from saved (game state) to VM (cart data)
        memcpy(mem + dest, saved + src, len);

        // Find __lua__ section bounds in original file for preservation
        const char *data_end = file_data + file_len;
        const char *lua_start = find_section(file_data, file_len, "__lua__");
        if (lua_start) {
            const char *lua_end = find_section_end(lua_start, data_end);
            lua_section_len = lua_end - lua_start;
            lua_section = (char *)tlsf_malloc(cart_tlsf, lua_section_len + 1);
            if (lua_section) {
                memcpy(lua_section, lua_start, lua_section_len);
                lua_section[lua_section_len] = '\0';
            }
        }

        if (lua_code) tlsf_free(cart_tlsf, lua_code);
        tlsf_free(cart_tlsf, file_data);
    } else {
        // No existing file — just copy game data range into VM at dest
        memcpy(mem + dest, saved + src, len);
    }

    // Now VM has the modified cart data — serialize to .p8 file
    // Max data section sizes:
    // gfx: 128*129=16512, gff: 2*257=514, map: 32*257=8224,
    // sfx: 64*169=10816, music: 64*13=832 ≈ 37000 + headers + lua
    size_t buf_size = 37000 + (lua_section_len ? lua_section_len : 1) + 256;
    char *out = (char *)tlsf_malloc(cart_tlsf, buf_size);
    if (!out) {
        if (lua_section) tlsf_free(cart_tlsf, lua_section);
        memcpy(mem, saved, 0x8000);
        tlsf_free(cart_tlsf, saved);
        audio_resume();
        return -1;
    }

    char *p = out;
    // Header
    memcpy(p, "pico-8 cartridge // http://www.pico-8.com\nversion 41\n", 53);
    p += 53;

    // __lua__
    memcpy(p, "__lua__\n", 8); p += 8;
    if (lua_section) {
        memcpy(p, lua_section, lua_section_len);
        p += lua_section_len;
        tlsf_free(cart_tlsf, lua_section);
    }
    if (p > out && p[-1] != '\n') *p++ = '\n';

    // __gfx__
    memcpy(p, "__gfx__\n", 8); p += 8;
    p += serialize_gfx(p, mem);

    // __gff__
    memcpy(p, "__gff__\n", 8); p += 8;
    p += serialize_gff(p, mem);

    // __map__
    memcpy(p, "__map__\n", 8); p += 8;
    p += serialize_map(p, mem);

    // __sfx__
    memcpy(p, "__sfx__\n", 8); p += 8;
    p += serialize_sfx(p, mem);

    // __music__
    memcpy(p, "__music__\n", 10); p += 10;
    p += serialize_music(p, mem);

    size_t out_len = (size_t)(p - out);

    // Restore virtual memory
    memcpy(mem, saved, 0x8000);
    tlsf_free(cart_tlsf, saved);

    // Write to SD via temp file
    static char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    static FIL fil;
    FRESULT fres = f_open(&fil, tmp_path, FA_WRITE | FA_CREATE_ALWAYS);
    int result = -1;
    if (fres == FR_OK) {
        UINT bw;
        fres = f_write(&fil, out, (UINT)out_len, &bw);
        if (fres == FR_OK && bw == (UINT)out_len) {
            f_sync(&fil);
            f_close(&fil);
            f_unlink(path);
            if (f_rename(tmp_path, path) == FR_OK) {
                result = 0;
            }
        } else {
            f_close(&fil);
            f_unlink(tmp_path);
        }
    }

    tlsf_free(cart_tlsf, out);
    audio_resume();
    return result;
}

// ============================================================
// Cart picker UI
// ============================================================

#define MAX_CARTS 64

static char cart_names[MAX_CARTS][64];
static int cart_count = 0;

static bool is_cart_file(const char *name) {
    size_t nlen = strlen(name);
    if (nlen >= 7 && strcmp(name + nlen - 7, ".p8.png") == 0) return true;
    if (nlen >= 3 && strcmp(name + nlen - 3, ".p8") == 0) return true;
    return false;
}

static void scan_dir(const char *dirpath, const char *prefix) {
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, dirpath) != FR_OK) return;
    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR) continue;
        if (is_cart_file(fno.fname) && cart_count < MAX_CARTS) {
            snprintf(cart_names[cart_count], 64, "%s%s", prefix, fno.fname);
            cart_count++;
        }
    }
    f_closedir(&dir);
}

static void scan_carts(void) {
    cart_count = 0;
    scan_dir("/", "/");
    scan_dir("/carts", "/carts/");
}

// Returns selected cart path, or NULL if user pressed ESC or no carts found
const char *p8_cart_picker(void) {
    scan_carts();

    if (cart_count == 0) {
        printf("No .p8 carts found on SD card\n");
        return NULL;
    }

    printf("Found %d cart(s)\n", cart_count);

    int selected = 0;
    int scroll = 0;
    const int visible = 18; // lines visible on 128px screen with 6px font + header

    while (true) {
        input_update();

        // Navigation
        if (input_btnp(2, 0)) { // up
            selected--;
            if (selected < 0) selected = cart_count - 1;
        }
        if (input_btnp(3, 0)) { // down
            selected++;
            if (selected >= cart_count) selected = 0;
        }
        if (input_btnp(4, 0) || input_btnp(5, 0)) { // O or X = select
            return cart_names[selected];
        }
        if (input_key(0x29)) { // ESC
            return NULL;
        }

        // Scroll to keep selected visible
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible) scroll = selected - visible + 1;

        // Draw picker UI
        gfx_cls(1); // dark blue background

        // Header
        gfx_print("wili8jam carts", 16, 2, 7);
        gfx_rectfill(0, 9, 127, 9, 6); // separator line

        // Cart list
        for (int i = 0; i < visible && scroll + i < cart_count; i++) {
            int idx = scroll + i;
            int y = 12 + i * 6;

            // Extract just the filename from the path
            const char *name = cart_names[idx];
            const char *slash = strrchr(name, '/');
            if (slash) name = slash + 1;

            if (idx == selected) {
                gfx_rectfill(0, y, 127, y + 5, 12); // blue highlight
                gfx_print(">", 0, y, 7);
                gfx_print(name, 6, y, 7);
            } else {
                gfx_print(name, 6, y, 6);
            }
        }

        // Footer
        gfx_print("o/x:run esc:repl", 8, 122, 5);

        gfx_flip();
        sleep_ms(33); // ~30fps
    }
}

// ============================================================
// Lua globals for cart lifecycle
// ============================================================

// Forward declaration — main.cpp will set this so we can access the Lua state
static lua_State *cart_lua_state = NULL;

static int p8_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char fullpath[256];
    FILINFO fno;

    // Search order: /name, /name.p8, /name.p8.png, /carts/name, /carts/name.p8, /carts/name.p8.png
    const char *prefixes[] = { "/", "/carts/" };
    const char *suffixes[] = { "", ".p8", ".p8.png" };

    for (int p = 0; p < 2; p++) {
        for (int s = 0; s < 3; s++) {
            if (path[0] == '/')
                snprintf(fullpath, sizeof(fullpath), "%s%s", path, suffixes[s]);
            else
                snprintf(fullpath, sizeof(fullpath), "%s%s%s", prefixes[p], path, suffixes[s]);
            if (f_stat(fullpath, &fno) == FR_OK) {
                p8_cart_load(L, fullpath);
                return 0;
            }
        }
    }

    return luaL_error(L, "cart not found: %s", path);
}

static int p8_reset(lua_State *L) {
    (void)L;
    // Reset PICO-8 draw state (palette, camera, clip, cursor, color, fill pattern)
    // This matches PICO-8 behavior: reset() resets draw state, not the cart
    p8_reset_draw_state();
    return 0;
}

static int p8_run_cmd(lua_State *L) {
    if (current_cart_path[0] != '\0') {
        p8_cart_run(L, current_cart_path);
    }
    return 0;
}

static int p8_resume_cmd(lua_State *L) {
    // Resume a stopped cart — re-enter game loop without calling _init()
    if (!has_global_func(L, "_update") && !has_global_func(L, "_update60")) {
        printf("Nothing to resume\n");
        p8_console_printf("nothing to resume\n");
        p8_console_draw(); gfx_flip();
        return 0;
    }
    // Re-enter game loop (skips _init, just runs _update/_draw)
    p8_cart_gameloop_resume(L);
    return 0;
}

// ============================================================
// Public API
// ============================================================

void p8_cart_init(tlsf_t tlsf) {
    cart_tlsf = tlsf;
    current_cart_path[0] = '\0';
    p8_png_init(tlsf);
}

void p8_cart_register(lua_State *L) {
    lua_register(L, "load", p8_load);
    lua_register(L, "reset", p8_reset);
    lua_register(L, "run", p8_run_cmd);
    lua_register(L, "resume", p8_resume_cmd);
    cart_lua_state = L;
}

const char *p8_cart_get_path(void) {
    return current_cart_path;
}
