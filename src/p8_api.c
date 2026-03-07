/*
 * PICO-8 API Layer for wili8jam
 *
 * Registers all PICO-8 standard library functions as Lua globals.
 * Wraps existing gfx.c, input.c, audio.c hardware drivers.
 */

#include "p8_api.h"
#include "p8_cart.h"
#include "p8_console.h"
#include "p8_sfx.h"
#include "gfx.h"
#include "input.h"
#include "audio.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tlsf/tlsf.h"
#include "pico/stdlib.h"
#include "fatfs/ff.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// PICO-8 Virtual Memory (32KB)
// ============================================================

#define P8_MEM_SIZE     0x8000

// Memory map offsets
#define P8_SPRITE_LO    0x0000  // 0x0000-0x0FFF: sprite sheet lower
#define P8_SPRITE_HI    0x1000  // 0x1000-0x1FFF: sprite sheet upper / map shared
#define P8_MAP_LO       0x2000  // 0x2000-0x2FFF: map lower
#define P8_FLAGS        0x3000  // 0x3000-0x307F: sprite flags (128 bytes)
#define P8_MUSIC        0x3100  // 0x3100-0x31FF: music patterns
#define P8_SFX          0x3200  // 0x3200-0x42FF: sfx data
#define P8_DRAWSTATE    0x5F00  // 0x5F00-0x5FFF: draw state
#define P8_SCREEN       0x6000  // 0x6000-0x7FFF: screen buffer

static uint8_t *p8_mem;
static tlsf_t p8_tlsf;

// ============================================================
// Draw state
// ============================================================

static int p8_draw_color = 6;
static int p8_cursor_x = 0;
static int p8_cursor_y = 0;
static int p8_camera_x = 0;
static int p8_camera_y = 0;
static int p8_clip_x = 0;
static int p8_clip_y = 0;
static int p8_clip_w = 128;
static int p8_clip_h = 128;

// Draw palette: maps PICO-8 color index to actual color drawn
static uint8_t p8_draw_pal[16];
// Display palette: maps framebuffer color to display color (applied at flip)
static uint8_t p8_display_pal[16];
// Transparency flags per color (bit = transparent)
static uint16_t p8_palt_mask = 0x0001; // color 0 transparent by default
// Fill pattern
static uint16_t p8_fill_pattern = 0;
static bool p8_fill_transparent = false;

// When true, flip() is lightweight (game loop handles timing/input)
static bool p8_gameloop_mode = false;

// Time tracking
static absolute_time_t p8_start_time;

// CPU usage tracking: fraction of frame time used by game logic (0.0-1.0+)
static float p8_cpu_usage = 0.0f;

// RNG state (simple xorshift32)
static uint32_t p8_rng_state = 0x12345678;

// Last printed string width (for stat(56))
static int p8_last_print_w = 0;

// Target FPS (30 or 60, set by cart loader)
static int p8_target_fps = 30;

// Cartdata
static bool p8_cartdata_open = false;
static char p8_cartdata_id[64];
static float p8_cartdata[64]; // using float since LUA_32BITS=1

// ============================================================
// Internal helpers
// ============================================================

static inline int p8_clamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Apply draw palette and check transparency. Returns -1 if transparent.
static inline int p8_resolve_color(int c) {
    c = p8_draw_pal[c & 0xF];
    if (p8_palt_mask & (1 << c)) return -1;
    return c;
}

// Check if fill pattern makes pixel transparent at (x,y)
static inline bool p8_fill_check(int x, int y) {
    if (p8_fill_pattern == 0) return false;
    int bit = (y & 3) * 4 + (x & 3);
    bool pattern_set = (p8_fill_pattern >> bit) & 1;
    // If pattern bit is set, the pixel uses the "alternate" behavior:
    // With fill_transparent=true, it becomes transparent
    // With fill_transparent=false, it uses draw color from secondary nibble (color 0)
    return pattern_set && p8_fill_transparent;
}

// Draw a single pixel with camera offset, clipping, palette, fill pattern
static inline void p8_pixel(int x, int y, int c) {
    x -= p8_camera_x;
    y -= p8_camera_y;
    if (x < p8_clip_x || x >= p8_clip_x + p8_clip_w) return;
    if (y < p8_clip_y || y >= p8_clip_y + p8_clip_h) return;
    if (p8_fill_check(x, y)) return;
    c = p8_draw_pal[c & 0xF];
    gfx_pset(x, y, c);
}

// Horizontal line with clipping/camera/palette
static void p8_hline(int x0, int x1, int y, int c) {
    y -= p8_camera_y;
    if (y < p8_clip_y || y >= p8_clip_y + p8_clip_h) return;
    x0 -= p8_camera_x;
    x1 -= p8_camera_x;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < p8_clip_x) x0 = p8_clip_x;
    if (x1 >= p8_clip_x + p8_clip_w) x1 = p8_clip_x + p8_clip_w - 1;
    c = p8_draw_pal[c & 0xF];
    for (int x = x0; x <= x1; x++) {
        if (!p8_fill_check(x, y))
            gfx_pset(x, y, c);
    }
}

// ============================================================
// Phase 1: Core globals
// ============================================================

// --- Graphics ---

static int p8_cls(lua_State *L) {
    int c = (int)luaL_optinteger(L, 1, 0);
    gfx_cls(c & 0xF);
    p8_cursor_x = 0;
    p8_cursor_y = 0;
    // In REPL mode, also clear the console so it doesn't overwrite the cls color
    if (!p8_gameloop_mode) {
        p8_console_clear();
        p8_console_set_bg(c & 0xF);
    }
    return 0;
}

static int p8_pset(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int c = lua_isnoneornil(L, 3) ? p8_draw_color : (int)luaL_checknumber(L, 3);
    p8_pixel(x, y, c);
    if (!lua_isnoneornil(L, 3)) p8_draw_color = c & 0xF;
    return 0;
}

static int p8_pget(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1) - p8_camera_x;
    int y = (int)luaL_checknumber(L, 2) - p8_camera_y;
    lua_pushinteger(L, gfx_pget(x, y));
    return 1;
}

static int p8_print(lua_State *L) {
    const char *str = luaL_tolstring(L, 1, NULL);
    lua_pop(L, 1); // pop the tolstring result
    if (!str) str = "";

    int x, y, c;
    if (lua_gettop(L) >= 3) {
        x = (int)luaL_checknumber(L, 2);
        y = (int)luaL_checknumber(L, 3);
        c = lua_isnoneornil(L, 4) ? p8_draw_color : (int)luaL_checknumber(L, 4);
    } else {
        x = p8_cursor_x;
        y = p8_cursor_y;
        c = lua_isnoneornil(L, 2) ? p8_draw_color : (int)luaL_checknumber(L, 2);
    }

    if (!lua_isnoneornil(L, 4) || (lua_gettop(L) < 3 && !lua_isnoneornil(L, 2)))
        p8_draw_color = c & 0xF;

    int draw_x = x - p8_camera_x;
    int draw_y = y - p8_camera_y;
    int tw = gfx_text_width(str, 4);
    gfx_print(str, draw_x, draw_y, p8_draw_pal[c & 0xF]);
    p8_cursor_x = x;
    p8_cursor_y = y + 6;
    p8_last_print_w = tw;
    lua_pushinteger(L, draw_x + tw);
    return 1;
}

static int p8_color(lua_State *L) {
    int c = (int)luaL_optinteger(L, 1, 6);
    p8_draw_color = c & 0xF;
    return 0;
}

static int p8_cursor(lua_State *L) {
    p8_cursor_x = (int)luaL_optnumber(L, 1, 0);
    p8_cursor_y = (int)luaL_optnumber(L, 2, 0);
    if (!lua_isnoneornil(L, 3))
        p8_draw_color = (int)luaL_checknumber(L, 3) & 0xF;
    return 0;
}

static int p8_camera(lua_State *L) {
    p8_camera_x = (int)luaL_optnumber(L, 1, 0);
    p8_camera_y = (int)luaL_optnumber(L, 2, 0);
    return 0;
}

static int p8_clip(lua_State *L) {
    if (lua_gettop(L) == 0) {
        p8_clip_x = 0;
        p8_clip_y = 0;
        p8_clip_w = 128;
        p8_clip_h = 128;
    } else {
        int x = (int)luaL_checknumber(L, 1);
        int y = (int)luaL_checknumber(L, 2);
        int w = (int)luaL_checknumber(L, 3);
        int h = (int)luaL_checknumber(L, 4);
        int clip_prev = lua_toboolean(L, 5);
        if (clip_prev) {
            // Intersect with current clip
            int nx = x > p8_clip_x ? x : p8_clip_x;
            int ny = y > p8_clip_y ? y : p8_clip_y;
            int nx2 = (x + w) < (p8_clip_x + p8_clip_w) ? (x + w) : (p8_clip_x + p8_clip_w);
            int ny2 = (y + h) < (p8_clip_y + p8_clip_h) ? (y + h) : (p8_clip_y + p8_clip_h);
            p8_clip_x = nx;
            p8_clip_y = ny;
            p8_clip_w = nx2 - nx > 0 ? nx2 - nx : 0;
            p8_clip_h = ny2 - ny > 0 ? ny2 - ny : 0;
        } else {
            p8_clip_x = x;
            p8_clip_y = y;
            p8_clip_w = w;
            p8_clip_h = h;
        }
    }
    return 0;
}

static int p8_line(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    int c = lua_isnoneornil(L, 5) ? p8_draw_color : (int)luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 5)) p8_draw_color = c & 0xF;

    // Bresenham with camera/clip
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    int err = dx - dy;
    int x = x0, y = y0;
    for (;;) {
        p8_pixel(x, y, c);
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
    return 0;
}

static int p8_rect(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    int c = lua_isnoneornil(L, 5) ? p8_draw_color : (int)luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 5)) p8_draw_color = c & 0xF;
    // Top and bottom
    p8_hline(x0, x1, y0, c);
    p8_hline(x0, x1, y1, c);
    // Left and right
    for (int y = y0 + 1; y < y1; y++) {
        p8_pixel(x0, y, c);
        p8_pixel(x1, y, c);
    }
    return 0;
}

static int p8_rectfill(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    int c = lua_isnoneornil(L, 5) ? p8_draw_color : (int)luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 5)) p8_draw_color = c & 0xF;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        p8_hline(x0, x1, y, c);
    return 0;
}

static int p8_circ(lua_State *L) {
    int cx = (int)luaL_checknumber(L, 1);
    int cy = (int)luaL_checknumber(L, 2);
    int r = (int)luaL_optnumber(L, 3, 4);
    int c = lua_isnoneornil(L, 4) ? p8_draw_color : (int)luaL_checknumber(L, 4);
    if (!lua_isnoneornil(L, 4)) p8_draw_color = c & 0xF;
    if (r < 0) return 0;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        p8_pixel(cx+x, cy+y, c); p8_pixel(cx-x, cy+y, c);
        p8_pixel(cx+x, cy-y, c); p8_pixel(cx-x, cy-y, c);
        p8_pixel(cx+y, cy+x, c); p8_pixel(cx-y, cy+x, c);
        p8_pixel(cx+y, cy-x, c); p8_pixel(cx-y, cy-x, c);
        y++;
        if (d < 0) d += 2*y + 1;
        else { x--; d += 2*(y - x) + 1; }
    }
    return 0;
}

static int p8_circfill(lua_State *L) {
    int cx = (int)luaL_checknumber(L, 1);
    int cy = (int)luaL_checknumber(L, 2);
    int r = (int)luaL_optnumber(L, 3, 4);
    int c = lua_isnoneornil(L, 4) ? p8_draw_color : (int)luaL_checknumber(L, 4);
    if (!lua_isnoneornil(L, 4)) p8_draw_color = c & 0xF;
    if (r < 0) return 0;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        p8_hline(cx-x, cx+x, cy+y, c);
        p8_hline(cx-x, cx+x, cy-y, c);
        p8_hline(cx-y, cx+y, cy+x, c);
        p8_hline(cx-y, cx+y, cy-x, c);
        y++;
        if (d < 0) d += 2*y + 1;
        else { x--; d += 2*(y - x) + 1; }
    }
    return 0;
}

static int p8_oval(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    int c = lua_isnoneornil(L, 5) ? p8_draw_color : (int)luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 5)) p8_draw_color = c & 0xF;
    // Midpoint ellipse algorithm
    float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
    float rx = (x1 - x0) / 2.0f, ry = (y1 - y0) / 2.0f;
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    if (rx < 0.5f && ry < 0.5f) { p8_pixel((int)cx, (int)cy, c); return 0; }
    float rx2 = rx*rx, ry2 = ry*ry;
    float x = 0, y = ry;
    float px = 0, py = 2*rx2*y;
    // Region 1
    float d1 = ry2 - rx2*ry + 0.25f*rx2;
    while (px < py) {
        p8_pixel((int)(cx+x), (int)(cy+y), c);
        p8_pixel((int)(cx-x), (int)(cy+y), c);
        p8_pixel((int)(cx+x), (int)(cy-y), c);
        p8_pixel((int)(cx-x), (int)(cy-y), c);
        x++; px += 2*ry2;
        if (d1 < 0) { d1 += ry2 + px; }
        else { y--; py -= 2*rx2; d1 += ry2 + px - py; }
    }
    // Region 2
    float d2 = ry2*(x+0.5f)*(x+0.5f) + rx2*(y-1)*(y-1) - rx2*ry2;
    while (y >= 0) {
        p8_pixel((int)(cx+x), (int)(cy+y), c);
        p8_pixel((int)(cx-x), (int)(cy+y), c);
        p8_pixel((int)(cx+x), (int)(cy-y), c);
        p8_pixel((int)(cx-x), (int)(cy-y), c);
        y--; py -= 2*rx2;
        if (d2 > 0) { d2 += rx2 - py; }
        else { x++; px += 2*ry2; d2 += rx2 - py + px; }
    }
    return 0;
}

static int p8_ovalfill(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    int c = lua_isnoneornil(L, 5) ? p8_draw_color : (int)luaL_checknumber(L, 5);
    if (!lua_isnoneornil(L, 5)) p8_draw_color = c & 0xF;
    float cxf = (x0 + x1) / 2.0f, cyf = (y0 + y1) / 2.0f;
    float rx = (x1 - x0) / 2.0f, ry = (y1 - y0) / 2.0f;
    if (rx < 0) rx = -rx;
    if (ry < 0) ry = -ry;
    int iy0 = (int)(cyf - ry), iy1 = (int)(cyf + ry);
    for (int iy = iy0; iy <= iy1; iy++) {
        float dy = (iy - cyf) / (ry > 0 ? ry : 1);
        if (dy*dy > 1.0f) continue;
        float dx = rx * sqrtf(1.0f - dy*dy);
        int lx = (int)(cxf - dx);
        int hx = (int)(cxf + dx);
        p8_hline(lx, hx, iy, c);
    }
    return 0;
}

static int p8_flip(lua_State *L) {
    (void)L;
    gfx_flip();
    if (!p8_gameloop_mode) {
        // REPL mode: flip handles timing and input polling
        sleep_ms(33);
        input_update();
    }
    // In game loop mode, the game loop handles timing and input
    return 0;
}

void p8_set_gameloop_mode(bool enabled) {
    p8_gameloop_mode = enabled;
}

void p8_set_cpu_usage(float usage) {
    p8_cpu_usage = usage;
}

void p8_set_target_fps(int fps) {
    p8_target_fps = fps;
}

void p8_register_print(lua_State *L) {
    lua_register(L, "print", p8_print);
}

// --- Sprite drawing ---

static int p8_spr(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    int dx = (int)luaL_optnumber(L, 2, 0);
    int dy = (int)luaL_optnumber(L, 3, 0);
    float w = (float)luaL_optnumber(L, 4, 1);
    float h = (float)luaL_optnumber(L, 5, 1);
    int flip_x = lua_toboolean(L, 6);
    int flip_y = lua_toboolean(L, 7);

    int pw = (int)(w * 8), ph = (int)(h * 8);
    // Sprite sheet is 16 sprites wide (128px / 8px)
    int sx = (n % 16) * 8;
    int sy = (n / 16) * 8;

    for (int py = 0; py < ph; py++) {
        for (int px = 0; px < pw; px++) {
            int src_x = sx + (flip_x ? (pw - 1 - px) : px);
            int src_y = sy + (flip_y ? (ph - 1 - py) : py);
            // Read pixel from sprite sheet in virtual memory
            // Sprite sheet: 2 pixels per byte, row-major, 64 bytes per row
            if (src_x < 0 || src_x >= 128 || src_y < 0 || src_y >= 128) continue;
            int addr = (src_y * 64) + (src_x / 2);
            uint8_t byte = p8_mem[addr];
            int c = (src_x & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
            // Check transparency
            if (p8_palt_mask & (1 << c)) continue;
            p8_pixel(dx + px, dy + py, c);
        }
    }
    return 0;
}

static int p8_sspr(lua_State *L) {
    int sx = (int)luaL_checknumber(L, 1);
    int sy = (int)luaL_checknumber(L, 2);
    int sw = (int)luaL_checknumber(L, 3);
    int sh = (int)luaL_checknumber(L, 4);
    int dx = (int)luaL_checknumber(L, 5);
    int dy = (int)luaL_checknumber(L, 6);
    int dw = (int)luaL_optnumber(L, 7, sw);
    int dh = (int)luaL_optnumber(L, 8, sh);
    int flip_x = lua_toboolean(L, 9);
    int flip_y = lua_toboolean(L, 10);

    for (int py = 0; py < dh; py++) {
        for (int px = 0; px < dw; px++) {
            int src_x = sx + (flip_x ? (sw - 1 - px * sw / dw) : (px * sw / dw));
            int src_y = sy + (flip_y ? (sh - 1 - py * sh / dh) : (py * sh / dh));
            if (src_x < 0 || src_x >= 128 || src_y < 0 || src_y >= 128) continue;
            int addr = (src_y * 64) + (src_x / 2);
            uint8_t byte = p8_mem[addr];
            int c = (src_x & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
            if (p8_palt_mask & (1 << c)) continue;
            p8_pixel(dx + px, dy + py, c);
        }
    }
    return 0;
}

// --- Map ---

static int p8_mget(lua_State *L) {
    int cx = (int)luaL_checknumber(L, 1);
    int cy = (int)luaL_checknumber(L, 2);
    if (cx < 0 || cx >= 128 || cy < 0 || cy >= 64) {
        lua_pushinteger(L, 0);
        return 1;
    }
    int addr;
    if (cy < 32)
        addr = P8_MAP_LO + cy * 128 + cx;  // 0x2000
    else
        addr = P8_SPRITE_HI + (cy - 32) * 128 + cx;  // 0x1000 (shared)
    lua_pushinteger(L, p8_mem[addr]);
    return 1;
}

static int p8_mset(lua_State *L) {
    int cx = (int)luaL_checknumber(L, 1);
    int cy = (int)luaL_checknumber(L, 2);
    int v = (int)luaL_checknumber(L, 3);
    if (cx < 0 || cx >= 128 || cy < 0 || cy >= 64) return 0;
    int addr;
    if (cy < 32)
        addr = P8_MAP_LO + cy * 128 + cx;
    else
        addr = P8_SPRITE_HI + (cy - 32) * 128 + cx;
    p8_mem[addr] = (uint8_t)v;
    return 0;
}

static int p8_map(lua_State *L) {
    int cx = (int)luaL_optnumber(L, 1, 0);
    int cy = (int)luaL_optnumber(L, 2, 0);
    int sx = (int)luaL_optnumber(L, 3, 0);
    int sy = (int)luaL_optnumber(L, 4, 0);
    int cw = (int)luaL_optnumber(L, 5, 128);
    int ch = (int)luaL_optnumber(L, 6, 64);
    int layer = (int)luaL_optnumber(L, 7, 0);

    for (int ty = 0; ty < ch; ty++) {
        for (int tx = 0; tx < cw; tx++) {
            int mcx = cx + tx, mcy = cy + ty;
            if (mcx < 0 || mcx >= 128 || mcy < 0 || mcy >= 64) continue;
            int addr;
            if (mcy < 32)
                addr = P8_MAP_LO + mcy * 128 + mcx;
            else
                addr = P8_SPRITE_HI + (mcy - 32) * 128 + mcx;
            int tile = p8_mem[addr];
            if (tile == 0) continue;  // tile 0 = empty (PICO-8 convention)
            // Layer filter: if layer != 0, check sprite flags
            if (layer != 0) {
                uint8_t flags = p8_mem[P8_FLAGS + tile];
                if (!(flags & layer)) continue;
            }
            // Draw the sprite at tile position
            int draw_x = sx + tx * 8;
            int draw_y = sy + ty * 8;
            int spr_sx = (tile % 16) * 8;
            int spr_sy = (tile / 16) * 8;
            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    int src_x = spr_sx + px;
                    int src_y = spr_sy + py;
                    int a = (src_y * 64) + (src_x / 2);
                    uint8_t byte = p8_mem[a];
                    int c = (src_x & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
                    if (p8_palt_mask & (1 << c)) continue;
                    p8_pixel(draw_x + px, draw_y + py, c);
                }
            }
        }
    }
    return 0;
}

// --- Input ---

static int p8_btn(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        // Return bitmask of all buttons for player 0
        int mask = 0;
        for (int i = 0; i < 6; i++)
            if (input_btn(i, 0)) mask |= (1 << i);
        lua_pushinteger(L, mask);
        return 1;
    }
    int i = (int)luaL_checknumber(L, 1);
    int p = (int)luaL_optnumber(L, 2, 0);
    lua_pushboolean(L, input_btn(i, p));
    return 1;
}

static int p8_btnp(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        int mask = 0;
        for (int i = 0; i < 6; i++)
            if (input_btnp(i, 0)) mask |= (1 << i);
        lua_pushinteger(L, mask);
        return 1;
    }
    int i = (int)luaL_checknumber(L, 1);
    int p = (int)luaL_optnumber(L, 2, 0);
    lua_pushboolean(L, input_btnp(i, p));
    return 1;
}

// --- Math (PICO-8 conventions) ---

static int p8_flr(lua_State *L) {
    lua_pushnumber(L, floorf((float)luaL_checknumber(L, 1)));
    return 1;
}

static int p8_ceil(lua_State *L) {
    lua_pushnumber(L, ceilf((float)luaL_checknumber(L, 1)));
    return 1;
}

static int p8_abs(lua_State *L) {
    lua_Number x = luaL_checknumber(L, 1);
    lua_pushnumber(L, x < 0 ? -x : x);
    return 1;
}

static int p8_max(lua_State *L) {
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_optnumber(L, 2, 0);
    lua_pushnumber(L, a > b ? a : b);
    return 1;
}

static int p8_min(lua_State *L) {
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_optnumber(L, 2, 0);
    lua_pushnumber(L, a < b ? a : b);
    return 1;
}

static int p8_mid(lua_State *L) {
    lua_Number x = luaL_checknumber(L, 1);
    lua_Number y = luaL_checknumber(L, 2);
    lua_Number z = luaL_checknumber(L, 3);
    lua_Number result;
    if (x > y) { lua_Number t = x; x = y; y = t; }
    // now x <= y
    if (z < x) result = x;
    else if (z > y) result = y;
    else result = z;
    lua_pushnumber(L, result);
    return 1;
}

// PICO-8 sin: sin(0.25) = -1, uses 0-1 turns
static int p8_sin(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    lua_pushnumber(L, -sinf(x * 2.0f * (float)M_PI));
    return 1;
}

// PICO-8 cos: cos(0) = 1, uses 0-1 turns
static int p8_cos(lua_State *L) {
    float x = (float)luaL_checknumber(L, 1);
    lua_pushnumber(L, cosf(x * 2.0f * (float)M_PI));
    return 1;
}

// PICO-8 atan2: returns 0-1 turns
static int p8_atan2(lua_State *L) {
    float dx = (float)luaL_checknumber(L, 1);
    float dy = (float)luaL_checknumber(L, 2);
    // PICO-8 atan2 takes (dx,dy) and returns 0-1
    // PICO-8 convention: atan2(1,0)=0.25, atan2(0,-1)=0.25 (up)
    // Standard atan2(y,x) but PICO-8 inverts y
    float a = atan2f(-dy, dx) / (2.0f * (float)M_PI);
    if (a < 0) a += 1.0f;
    lua_pushnumber(L, a);
    return 1;
}

static int p8_sqrt(lua_State *L) {
    lua_pushnumber(L, sqrtf((float)luaL_checknumber(L, 1)));
    return 1;
}

static int p8_sgn(lua_State *L) {
    lua_Number x = luaL_checknumber(L, 1);
    lua_pushnumber(L, x < 0 ? -1 : 1); // sgn(0) = 1 in PICO-8
    return 1;
}

static uint32_t p8_rng_next(void) {
    uint32_t x = p8_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p8_rng_state = x;
    return x;
}

static int p8_rnd(lua_State *L) {
    if (lua_istable(L, 1)) {
        // Pick random element from table
        lua_Integer len = luaL_len(L, 1);
        if (len <= 0) { lua_pushnil(L); return 1; }
        int idx = (int)(p8_rng_next() % (uint32_t)len) + 1;
        lua_rawgeti(L, 1, idx);
        return 1;
    }
    float x = lua_isnoneornil(L, 1) ? 1.0f : (float)luaL_checknumber(L, 1);
    float r = (float)(p8_rng_next() & 0xFFFF) / 65536.0f;
    lua_pushnumber(L, r * x);
    return 1;
}

static int p8_srand(lua_State *L) {
    uint32_t s = (uint32_t)luaL_checknumber(L, 1);
    if (s == 0) s = 1;
    p8_rng_state = s;
    return 0;
}

// --- Table functions ---

// add(t, v, [i]) — insert v into table t
static int p8_add(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    if (lua_isnoneornil(L, 3)) {
        // Append
        lua_pushvalue(L, 2);
        lua_rawseti(L, 1, n + 1);
    } else {
        int i = (int)luaL_checkinteger(L, 3);
        // Shift elements up
        for (int j = n; j >= i; j--) {
            lua_rawgeti(L, 1, j);
            lua_rawseti(L, 1, j + 1);
        }
        lua_pushvalue(L, 2);
        lua_rawseti(L, 1, i);
    }
    lua_pushvalue(L, 2); // return the added value
    return 1;
}

// del(t, v) — delete first occurrence of v
static int p8_del(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_rawequal(L, -1, 2)) {
            lua_pop(L, 1);
            // Shift elements down
            for (int j = i; j < n; j++) {
                lua_rawgeti(L, 1, j + 1);
                lua_rawseti(L, 1, j);
            }
            lua_pushnil(L);
            lua_rawseti(L, 1, n);
            lua_pushvalue(L, 2); // return deleted value
            return 1;
        }
        lua_pop(L, 1);
    }
    return 0;
}

// deli(t, [i]) — delete by index
static int p8_deli(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    int i = (int)luaL_optinteger(L, 2, n);
    if (i < 1 || i > n) return 0;
    lua_rawgeti(L, 1, i); // return value
    for (int j = i; j < n; j++) {
        lua_rawgeti(L, 1, j + 1);
        lua_rawseti(L, 1, j);
    }
    lua_pushnil(L);
    lua_rawseti(L, 1, n);
    return 1;
}

// count(t, [v]) — count elements or occurrences
static int p8_count(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, n);
        return 1;
    }
    int cnt = 0;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_rawequal(L, -1, 2)) cnt++;
        lua_pop(L, 1);
    }
    lua_pushinteger(L, cnt);
    return 1;
}

// all(t) — iterator safe for deletion during iteration
// Returns an iterator function that captures the table and an index upvalue
static int p8_all_iter(lua_State *L) {
    int i = (int)lua_tointeger(L, lua_upvalueindex(2));
    int n = (int)luaL_len(L, lua_upvalueindex(1));
    i++;
    if (i > n) return 0;
    lua_pushinteger(L, i);
    lua_replace(L, lua_upvalueindex(2)); // update index
    lua_rawgeti(L, lua_upvalueindex(1), i);
    return 1;
}

static int p8_all(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);       // table as upvalue 1
    lua_pushinteger(L, 0);     // index as upvalue 2
    lua_pushcclosure(L, p8_all_iter, 2);
    return 1;
}

// foreach(t, f) — re-check length each iteration (table may be modified by callback)
static int p8_foreach(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int i = 1;
    while (i <= (int)luaL_len(L, 1)) {
        lua_pushvalue(L, 2);
        lua_rawgeti(L, 1, i);
        lua_call(L, 1, 0);
        i++;
    }
    return 0;
}

// --- String functions ---

static int p8_sub(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    int i = (int)luaL_checkinteger(L, 2);
    int j = (int)luaL_optinteger(L, 3, -1);
    // Convert PICO-8 1-based indices, negative wraps
    if (i < 0) i = (int)len + i + 1;
    if (j < 0) j = (int)len + j + 1;
    if (i < 1) i = 1;
    if (j > (int)len) j = (int)len;
    if (i > j) { lua_pushliteral(L, ""); return 1; }
    lua_pushlstring(L, s + i - 1, j - i + 1);
    return 1;
}

static int p8_chr(lua_State *L) {
    int n = (int)luaL_checkinteger(L, 1);
    char c = (char)(n & 0xFF);
    lua_pushlstring(L, &c, 1);
    return 1;
}

static int p8_ord(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    int i = (int)luaL_optinteger(L, 2, 1);
    int n = (int)luaL_optinteger(L, 3, 1);
    if (i < 1) i = 1;
    int count = 0;
    for (int k = 0; k < n; k++) {
        int idx = i + k - 1;
        if (idx < 0 || idx >= (int)len) break;
        lua_pushinteger(L, (unsigned char)s[idx]);
        count++;
    }
    return count;
}

static int p8_tostr(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushliteral(L, "[nil]");
        return 1;
    }
    int flags = (int)luaL_optinteger(L, 2, 0);
    if (lua_isboolean(L, 1)) {
        lua_pushstring(L, lua_toboolean(L, 1) ? "true" : "false");
        return 1;
    }
    if (lua_isnumber(L, 1)) {
        lua_Number v = lua_tonumber(L, 1);
        char buf[32];
        if (flags & 0x1) {
            // Hex format
            int iv = (int)v;
            snprintf(buf, sizeof(buf), "0x%04x", (unsigned)(iv & 0xFFFF));
        } else {
            snprintf(buf, sizeof(buf), "%g", (double)v);
        }
        lua_pushstring(L, buf);
        return 1;
    }
    // Default: use Lua's tostring
    luaL_tolstring(L, 1, NULL);
    return 1;
}

static int p8_tonum(lua_State *L) {
    if (lua_isnumber(L, 1)) {
        lua_pushvalue(L, 1);
        return 1;
    }
    const char *s = luaL_optstring(L, 1, NULL);
    if (!s) { lua_pushnil(L); return 1; }
    int flags = (int)luaL_optinteger(L, 2, 0);
    char *end;
    double v;
    if (flags & 0x1) {
        v = strtol(s, &end, 16);
    } else {
        v = strtod(s, &end);
    }
    if (end == s) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)v);
    return 1;
}

static int p8_split(lua_State *L) {
    size_t slen;
    const char *str = luaL_checklstring(L, 1, &slen);
    const char *sep = luaL_optstring(L, 2, ",");
    int convert = lua_isnoneornil(L, 3) ? 1 : lua_toboolean(L, 3);
    size_t seplen = strlen(sep);

    lua_newtable(L);
    int idx = 1;
    const char *p = str;
    const char *end = str + slen;

    if (seplen == 0) {
        // Split into individual characters
        for (size_t i = 0; i < slen; i++) {
            lua_pushlstring(L, str + i, 1);
            lua_rawseti(L, -2, idx++);
        }
        return 1;
    }

    while (p <= end) {
        const char *found = NULL;
        for (const char *s = p; s <= end - seplen; s++) {
            if (memcmp(s, sep, seplen) == 0) { found = s; break; }
        }
        size_t toklen = found ? (size_t)(found - p) : (size_t)(end - p);
        if (convert) {
            // Try to convert to number
            char *nend;
            double v = strtod(p, &nend);
            if (nend == p + toklen && toklen > 0) {
                lua_pushnumber(L, (lua_Number)v);
            } else {
                lua_pushlstring(L, p, toklen);
            }
        } else {
            lua_pushlstring(L, p, toklen);
        }
        lua_rawseti(L, -2, idx++);
        if (!found) break;
        p = found + seplen;
    }
    return 1;
}

// type() — same as Lua's type(), but registered as global
static int p8_type(lua_State *L) {
    luaL_checkany(L, 1);
    lua_pushstring(L, luaL_typename(L, 1));
    return 1;
}

// --- System ---

static int p8_time(lua_State *L) {
    (void)L;
    int64_t us = absolute_time_diff_us(p8_start_time, get_absolute_time());
    lua_pushnumber(L, (lua_Number)(us / 1000000.0));
    return 1;
}

static int p8_stat(lua_State *L) {
    int n = (int)luaL_checkinteger(L, 1);
    switch (n) {
    case 0: // Memory usage (KB)
        lua_pushnumber(L, (lua_Number)(lua_gc(L, LUA_GCCOUNT, 0)));
        break;
    case 1: // CPU usage (fraction of frame)
        lua_pushnumber(L, (lua_Number)p8_cpu_usage);
        break;
    case 4: // Clipboard (not supported on bare metal)
        lua_pushstring(L, "");
        break;
    case 5: // PICO-8 version
        lua_pushinteger(L, 42); // approximate PICO-8 0.2.5
        break;
    case 6: // Parameter string
        lua_pushstring(L, "");
        break;
    case 7: // Current framerate target
        lua_pushinteger(L, p8_target_fps);
        break;
    case 8: // Target FPS
        lua_pushinteger(L, p8_target_fps);
        break;
    // stat(16-19): SFX playing on channel 0-3
    case 16: case 17: case 18: case 19:
        lua_pushinteger(L, p8_sfx_get_current(n - 16));
        break;
    // stat(20-23): Note index on channel 0-3
    case 20: case 21: case 22: case 23:
        lua_pushinteger(L, p8_sfx_get_note(n - 20));
        break;
    case 24: // Current music pattern
        lua_pushinteger(L, p8_music_get_pattern());
        break;
    case 25: // Music pattern count
        lua_pushinteger(L, p8_music_get_count());
        break;
    case 26: // Ticks played on current music pattern
        lua_pushinteger(L, 0);
        break;
    // stat(32-36): Mouse input via USB mouse on PIO-USB host
    case 32: lua_pushinteger(L, input_mouse_x()); break;
    case 33: lua_pushinteger(L, input_mouse_y()); break;
    case 34: lua_pushinteger(L, input_mouse_buttons()); break;
    case 35: lua_pushinteger(L, 0); break; // mouse wheel X (USB mice only have Y)
    case 36: lua_pushinteger(L, input_mouse_wheel()); break;
    // stat(46-49): SFX playing on channel (alternative query)
    case 46: case 47: case 48: case 49:
        lua_pushinteger(L, p8_sfx_get_current(n - 46));
        break;
    case 56: // Width of last printed string
        lua_pushinteger(L, p8_last_print_w);
        break;
    case 57: // Height of last printed string
        lua_pushinteger(L, 6);
        break;
    case 100: // Breadcrumb / cart ID (not supported)
        lua_pushstring(L, "");
        break;
    default:
        lua_pushinteger(L, 0);
        break;
    }
    return 1;
}

static int p8_printh(lua_State *L) {
    const char *s = luaL_tolstring(L, 1, NULL);
    lua_pop(L, 1);
    if (s) printf("%s\n", s);
    return 0;
}

static int p8_stop(lua_State *L) {
    const char *msg = luaL_optstring(L, 1, NULL);
    if (msg) printf("%s\n", msg);
    return luaL_error(L, "cart stopped");
}

// sfx(n, [channel], [offset], [length])
// n=-1: stop all, n=-2: stop music channels
static int p8_sfx(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    if (n < 0) {
        p8_sfx_stop(n);
        return 0;
    }
    int channel = (int)luaL_optnumber(L, 2, -1);
    int offset  = (int)luaL_optnumber(L, 3, 0);
    int length  = (int)luaL_optnumber(L, 4, 0);
    p8_sfx_play(n, channel, offset, length);
    return 0;
}

// music(n, [fade_len], [channel_mask])
// n=-1: stop music
static int p8_music(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    int fade_len = (int)luaL_optnumber(L, 2, 0);
    int channel_mask = (int)luaL_optnumber(L, 3, 0xF);
    if (n < 0) {
        p8_music_stop();
    } else {
        p8_music_play(n, fade_len, channel_mask);
    }
    return 0;
}
// menuitem(idx, [label], [callback]) — store menu items for pause menu
// idx: 1-5, label: string, callback: function or nil to remove
static int p8_menuitem_refs[5] = { LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF };
static int p8_menuitem(lua_State *L) {
    int idx = (int)luaL_checkinteger(L, 1);
    if (idx < 1 || idx > 5) return 0;
    // Free old ref
    if (p8_menuitem_refs[idx-1] != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, p8_menuitem_refs[idx-1]);
        p8_menuitem_refs[idx-1] = LUA_NOREF;
    }
    // Store callback if provided
    if (lua_isfunction(L, 3)) {
        lua_pushvalue(L, 3);
        p8_menuitem_refs[idx-1] = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// extcmd(cmd) — system commands (screenshot, video, etc.)
static int p8_extcmd(lua_State *L) {
    const char *cmd = luaL_optstring(L, 1, "");
    if (strcmp(cmd, "reset") == 0) {
        // Software reset — same as reboot REPL command
        // Can't easily reboot from here, just print
        printf("extcmd(\"reset\"): use reboot command\n");
    }
    // screenshot, video, rec, label, etc. — silently ignore
    return 0;
}

// serial(channel, address, len) — GPIO/SPI serial interface (not supported)
static int p8_serial(lua_State *L) {
    (void)L;
    lua_pushinteger(L, 0);
    return 1;
}
static int p8_reload(lua_State *L) {
    int dest = (int)luaL_optnumber(L, 1, 0);
    int src = (int)luaL_optnumber(L, 2, 0);
    int len = (int)luaL_optnumber(L, 3, 0x4300);
    const char *filename = luaL_optstring(L, 4, NULL);
    p8_cart_reload(dest, src, len, filename);
    return 0;
}
static int p8_cstore(lua_State *L) {
    int dest = (int)luaL_optnumber(L, 1, 0);
    int src = (int)luaL_optnumber(L, 2, 0);
    int len = (int)luaL_optnumber(L, 3, 0x4300);
    const char *filename = luaL_optstring(L, 4, NULL);
    p8_cart_cstore(dest, src, len, filename);
    return 0;
}
static int p8_tline(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    float mx = (float)luaL_optnumber(L, 5, 0);
    float my = (float)luaL_optnumber(L, 6, 0);
    float mdx = (float)luaL_optnumber(L, 7, 0.125f); // 1/8 tile per pixel
    float mdy = (float)luaL_optnumber(L, 8, 0);
    int layers = (int)luaL_optnumber(L, 9, 0);

    // Bresenham line walk from (x0,y0) to (x1,y1) in screen space
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    int err = dx - dy;
    int x = x0, y = y0;

    for (;;) {
        // Convert texture coords to sprite sheet pixel
        int tx, ty;
        int cell_x = (int)floorf(mx);
        int cell_y = (int)floorf(my);
        float fx = mx - (float)cell_x;
        float fy = my - (float)cell_y;
        int px = (int)(fx * 8.0f) & 7;
        int py = (int)(fy * 8.0f) & 7;

        int tile;
        if (layers & 0x1000) {
            // Direct sprite sheet mode: treat spritesheet as 16x16 grid of tiles
            tile = (((cell_y & 0xF) * 16) + (cell_x & 0xF));
        } else {
            // Map mode: look up tile from map
            int mcx = ((cell_x % 128) + 128) % 128;
            int mcy = ((cell_y % 64) + 64) % 64;
            int addr;
            if (mcy < 32)
                addr = P8_MAP_LO + mcy * 128 + mcx;
            else
                addr = P8_SPRITE_HI + (mcy - 32) * 128 + mcx;
            tile = p8_mem[addr];

            // Layer filter (excluding the 0x1000 flag)
            if ((layers & 0xFF) != 0) {
                uint8_t flags = p8_mem[P8_FLAGS + tile];
                if (!(flags & (layers & 0xFF)))
                    goto tline_next;
            }
        }

        // Read pixel from sprite sheet
        tx = (tile % 16) * 8 + px;
        ty = (tile / 16) * 8 + py;
        {
            int saddr = ty * 64 + tx / 2;
            uint8_t byte = p8_mem[saddr];
            int c = (tx & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
            if (!(p8_palt_mask & (1 << c)))
                p8_pixel(x, y, c);
        }

    tline_next:
        if (x == x1 && y == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
        mx += mdx;
        my += mdy;
    }
    return 0;
}

// rrect / rrectfill — rounded rectangle (draws as regular rect, radius ignored for now)
static int p8_rrect(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int max_r = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 2;
    int r = lua_gettop(L) >= 5 ? (int)luaL_checknumber(L, 5) : max_r;
    if (r > max_r) r = max_r;
    if (r < 0) r = 0;
    int c = lua_gettop(L) >= 6 ? (int)lua_tonumber(L, 6) : p8_draw_color;
    p8_draw_color = c & 0xF;
    c &= 0xF;

    // Straight edges (inset by radius)
    p8_hline(x0 + r, x1 - r, y0, c); // top
    p8_hline(x0 + r, x1 - r, y1, c); // bottom
    for (int y = y0 + r; y <= y1 - r; y++) {
        p8_pixel(x0, y, c); // left
        p8_pixel(x1, y, c); // right
    }

    // Quarter-circle corners (midpoint algorithm)
    if (r > 0) {
        int cx0 = x0 + r, cy0 = y0 + r; // top-left center
        int cx1 = x1 - r, cy1 = y1 - r; // bottom-right center
        int px = r, py = 0, d = 1 - r;
        while (px >= py) {
            p8_pixel(cx1 + px, cy1 + py, c); // BR
            p8_pixel(cx1 + py, cy1 + px, c);
            p8_pixel(cx0 - px, cy1 + py, c); // BL
            p8_pixel(cx0 - py, cy1 + px, c);
            p8_pixel(cx1 + px, cy0 - py, c); // TR
            p8_pixel(cx1 + py, cy0 - px, c);
            p8_pixel(cx0 - px, cy0 - py, c); // TL
            p8_pixel(cx0 - py, cy0 - px, c);
            py++;
            if (d < 0) {
                d += 2 * py + 1;
            } else {
                px--;
                d += 2 * (py - px) + 1;
            }
        }
    }
    return 0;
}

static int p8_rrectfill(lua_State *L) {
    int x0 = (int)luaL_checknumber(L, 1);
    int y0 = (int)luaL_checknumber(L, 2);
    int x1 = (int)luaL_checknumber(L, 3);
    int y1 = (int)luaL_checknumber(L, 4);
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    int max_r = ((x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0)) / 2;
    int r = lua_gettop(L) >= 5 ? (int)luaL_checknumber(L, 5) : max_r;
    if (r > max_r) r = max_r;
    if (r < 0) r = 0;
    int c = lua_gettop(L) >= 6 ? (int)lua_tonumber(L, 6) : p8_draw_color;
    p8_draw_color = c & 0xF;
    c &= 0xF;

    // Fill the center rectangle (between corner rows)
    for (int y = y0 + r; y <= y1 - r; y++)
        p8_hline(x0, x1, y, c);

    // Fill top and bottom bands with rounded corners
    if (r > 0) {
        int cx0 = x0 + r, cx1 = x1 - r;
        int px = r, py = 0, d = 1 - r;
        while (px >= py) {
            // Top rounded rows
            p8_hline(cx0 - px, cx1 + px, y0 + r - py, c);
            if (px != py)
                p8_hline(cx0 - py, cx1 + py, y0 + r - px, c);
            // Bottom rounded rows
            p8_hline(cx0 - px, cx1 + px, y1 - r + py, c);
            if (px != py)
                p8_hline(cx0 - py, cx1 + py, y1 - r + px, c);
            py++;
            if (d < 0) {
                d += 2 * py + 1;
            } else {
                px--;
                d += 2 * (py - px) + 1;
            }
        }
    }
    return 0;
}

// --- Coroutine wrappers ---

static int p8_cocreate(lua_State *L) {
    lua_getglobal(L, "coroutine");
    lua_getfield(L, -1, "create");
    lua_remove(L, -2);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

static int p8_coresume(lua_State *L) {
    lua_getglobal(L, "coroutine");
    lua_getfield(L, -1, "resume");
    lua_remove(L, -2);
    int nargs = lua_gettop(L);
    for (int i = 1; i <= nargs; i++)
        lua_pushvalue(L, i);
    lua_call(L, nargs, LUA_MULTRET);
    return lua_gettop(L) - nargs;
}

static int p8_costatus(lua_State *L) {
    lua_getglobal(L, "coroutine");
    lua_getfield(L, -1, "status");
    lua_remove(L, -2);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    return 1;
}

static int p8_yield(lua_State *L) {
    return lua_yield(L, lua_gettop(L));
}

// --- Bitwise operations ---
// PICO-8 bitwise ops work on the full 32-bit integer representation

static int p8_band(lua_State *L) {
    int a = (int)luaL_checknumber(L, 1);
    int b = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(a & b));
    return 1;
}

static int p8_bor(lua_State *L) {
    int a = (int)luaL_checknumber(L, 1);
    int b = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(a | b));
    return 1;
}

static int p8_bxor(lua_State *L) {
    int a = (int)luaL_checknumber(L, 1);
    int b = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(a ^ b));
    return 1;
}

static int p8_bnot(lua_State *L) {
    int a = (int)luaL_checknumber(L, 1);
    lua_pushnumber(L, (lua_Number)(~a));
    return 1;
}

static int p8_shl(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(x << n));
    return 1;
}

static int p8_shr(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(x >> n)); // arithmetic shift (signed)
    return 1;
}

static int p8_lshr(lua_State *L) {
    unsigned int x = (unsigned int)(int)luaL_checknumber(L, 1);
    int n = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(int)(x >> n)); // logical shift
    return 1;
}

static int p8_rotl(lua_State *L) {
    uint32_t x = (uint32_t)(int)luaL_checknumber(L, 1);
    int n = (int)luaL_checknumber(L, 2) & 31;
    lua_pushnumber(L, (lua_Number)(int)((x << n) | (x >> (32 - n))));
    return 1;
}

static int p8_rotr(lua_State *L) {
    uint32_t x = (uint32_t)(int)luaL_checknumber(L, 1);
    int n = (int)luaL_checknumber(L, 2) & 31;
    lua_pushnumber(L, (lua_Number)(int)((x >> n) | (x << (32 - n))));
    return 1;
}

// ============================================================
// Phase 2: Sprite sheet, flags, palette
// ============================================================

static int p8_sget(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    if (x < 0 || x >= 128 || y < 0 || y >= 128) {
        lua_pushinteger(L, 0);
        return 1;
    }
    int addr = (y * 64) + (x / 2);
    uint8_t byte = p8_mem[addr];
    int c = (x & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
    lua_pushinteger(L, c);
    return 1;
}

static int p8_sset(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int c = (int)luaL_optnumber(L, 3, p8_draw_color) & 0xF;
    if (x < 0 || x >= 128 || y < 0 || y >= 128) return 0;
    int addr = (y * 64) + (x / 2);
    if (x & 1)
        p8_mem[addr] = (p8_mem[addr] & 0x0F) | (c << 4);
    else
        p8_mem[addr] = (p8_mem[addr] & 0xF0) | c;
    return 0;
}

static int p8_fget(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    if (n < 0 || n >= 256) { lua_pushinteger(L, 0); return 1; }
    uint8_t flags = p8_mem[P8_FLAGS + n];
    if (lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, flags);
    } else {
        int f = (int)luaL_checknumber(L, 2);
        lua_pushboolean(L, (flags >> f) & 1);
    }
    return 1;
}

static int p8_fset(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    if (n < 0 || n >= 256) return 0;
    if (lua_isboolean(L, 3)) {
        int f = (int)luaL_checknumber(L, 2);
        int v = lua_toboolean(L, 3);
        if (v)
            p8_mem[P8_FLAGS + n] |= (1 << f);
        else
            p8_mem[P8_FLAGS + n] &= ~(1 << f);
    } else {
        p8_mem[P8_FLAGS + n] = (uint8_t)(int)luaL_checknumber(L, 2);
    }
    return 0;
}

static int p8_pal(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        // Reset all palettes
        for (int i = 0; i < 16; i++) {
            p8_draw_pal[i] = i;
            p8_display_pal[i] = i;
        }
        p8_palt_mask = 0x0001;
        return 0;
    }
    if (lua_istable(L, 1)) {
        // Table form: pal(tbl, [p])
        int p = (int)luaL_optnumber(L, 2, 0);
        uint8_t *target = (p == 1) ? p8_display_pal : p8_draw_pal;
        for (int i = 0; i < 16; i++) {
            lua_rawgeti(L, 1, i);
            if (!lua_isnil(L, -1))
                target[i] = (uint8_t)((int)lua_tointeger(L, -1) & 0xF);
            lua_pop(L, 1);
        }
        return 0;
    }
    int c0 = (int)luaL_checknumber(L, 1) & 0xF;
    int c1 = (int)luaL_optnumber(L, 2, 0) & 0xF;
    int p = (int)luaL_optnumber(L, 3, 0);
    if (p == 1)
        p8_display_pal[c0] = c1;
    else
        p8_draw_pal[c0] = c1;
    return 0;
}

static int p8_palt(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        p8_palt_mask = 0x0001; // reset: only color 0 transparent
        return 0;
    }
    int c = (int)luaL_checknumber(L, 1) & 0xF;
    int t = lua_toboolean(L, 2);
    if (t)
        p8_palt_mask |= (1 << c);
    else
        p8_palt_mask &= ~(1 << c);
    return 0;
}

static int p8_fillp(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        p8_fill_pattern = 0;
        p8_fill_transparent = false;
        return 0;
    }
    lua_Number p = luaL_checknumber(L, 1);
    // PICO-8 convention: fillp(pattern + 0.5) enables transparency.
    // Threshold 0.25 instead of 0.5 to tolerate 32-bit float rounding.
    int ip = (int)p;
    if (p < 0) ip = (int)(p - 1); // floor for negative values
    p8_fill_pattern = (uint16_t)(ip & 0xFFFF);
    double frac = p - (double)ip;
    if (frac < 0) frac = -frac;
    p8_fill_transparent = (frac >= 0.25);
    return 0;
}

// ============================================================
// Phase 3: Memory access
// ============================================================

static int p8_peek(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_optnumber(L, 2, 1);
    if (n == 1) {
        if (addr < 0 || addr >= P8_MEM_SIZE) { lua_pushinteger(L, 0); return 1; }
        lua_pushinteger(L, p8_mem[addr]);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        int a = addr + i;
        if (a < 0 || a >= P8_MEM_SIZE)
            lua_pushinteger(L, 0);
        else
            lua_pushinteger(L, p8_mem[a]);
    }
    return n;
}

static int p8_poke(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int nargs = lua_gettop(L);
    for (int i = 2; i <= nargs; i++) {
        int a = addr + (i - 2);
        if (a >= 0 && a < P8_MEM_SIZE)
            p8_mem[a] = (uint8_t)(int)luaL_checknumber(L, i);
    }
    return 0;
}

static int p8_peek2(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_optnumber(L, 2, 1);
    for (int i = 0; i < n; i++) {
        int a = addr + i * 2;
        if (a < 0 || a + 1 >= P8_MEM_SIZE) {
            lua_pushinteger(L, 0);
        } else {
            lua_pushinteger(L, p8_mem[a] | (p8_mem[a+1] << 8));
        }
    }
    return n;
}

static int p8_poke2(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int val = (int)luaL_checknumber(L, 2);
    if (addr >= 0 && addr + 1 < P8_MEM_SIZE) {
        p8_mem[addr] = val & 0xFF;
        p8_mem[addr+1] = (val >> 8) & 0xFF;
    }
    return 0;
}

static int p8_peek4(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_optnumber(L, 2, 1);
    for (int i = 0; i < n; i++) {
        int a = addr + i * 4;
        if (a < 0 || a + 3 >= P8_MEM_SIZE) {
            lua_pushinteger(L, 0);
        } else {
            uint32_t v = p8_mem[a] | (p8_mem[a+1]<<8) | (p8_mem[a+2]<<16) | (p8_mem[a+3]<<24);
            lua_pushinteger(L, (lua_Integer)(int32_t)v);
        }
    }
    return n;
}

static int p8_poke4(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int32_t val = (int32_t)luaL_checknumber(L, 2);
    if (addr >= 0 && addr + 3 < P8_MEM_SIZE) {
        p8_mem[addr]   = val & 0xFF;
        p8_mem[addr+1] = (val >> 8) & 0xFF;
        p8_mem[addr+2] = (val >> 16) & 0xFF;
        p8_mem[addr+3] = (val >> 24) & 0xFF;
    }
    return 0;
}

static int p8_memcpy(lua_State *L) {
    int dest = (int)luaL_checknumber(L, 1);
    int src = (int)luaL_checknumber(L, 2);
    int len = (int)luaL_checknumber(L, 3);
    if (dest < 0 || src < 0 || len <= 0) return 0;
    if (dest + len > P8_MEM_SIZE) len = P8_MEM_SIZE - dest;
    if (src + len > P8_MEM_SIZE) len = P8_MEM_SIZE - src;
    memmove(&p8_mem[dest], &p8_mem[src], len);
    return 0;
}

static int p8_memset(lua_State *L) {
    int dest = (int)luaL_checknumber(L, 1);
    int val = (int)luaL_checknumber(L, 2);
    int len = (int)luaL_checknumber(L, 3);
    if (dest < 0 || len <= 0) return 0;
    if (dest + len > P8_MEM_SIZE) len = P8_MEM_SIZE - dest;
    memset(&p8_mem[dest], val & 0xFF, len);
    return 0;
}

// ============================================================
// Phase 4: Cartridge data persistence
// ============================================================

static void cartdata_save(void) {
    if (!p8_cartdata_open) return;
    static char cd_path[128];
    snprintf(cd_path, sizeof(cd_path), "/cartdata/%s.dat", p8_cartdata_id);
    static FIL cd_fil;  // static to avoid ~600 bytes on stack
    if (f_open(&cd_fil, cd_path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT bw;
        f_write(&cd_fil, p8_cartdata, sizeof(p8_cartdata), &bw);
        f_close(&cd_fil);
    }
}

static int p8_cartdata_fn(lua_State *L) {
    const char *id = luaL_checkstring(L, 1);
    snprintf(p8_cartdata_id, sizeof(p8_cartdata_id), "%s", id);
    memset(p8_cartdata, 0, sizeof(p8_cartdata));

    // Try to load existing data
    char path[128];
    snprintf(path, sizeof(path), "/cartdata/%s.dat", id);
    FIL fil;
    if (f_open(&fil, path, FA_READ) == FR_OK) {
        UINT br;
        f_read(&fil, p8_cartdata, sizeof(p8_cartdata), &br);
        f_close(&fil);
    } else {
        // Ensure directory exists
        f_mkdir("/cartdata");
    }
    p8_cartdata_open = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int p8_dget(lua_State *L) {
    int i = (int)luaL_checknumber(L, 1);
    if (i < 0 || i >= 64 || !p8_cartdata_open) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, p8_cartdata[i]);
    return 1;
}

static int p8_dset(lua_State *L) {
    int i = (int)luaL_checknumber(L, 1);
    float v = (float)luaL_checknumber(L, 2);
    if (i >= 0 && i < 64 && p8_cartdata_open) {
        p8_cartdata[i] = v;
        cartdata_save();
    }
    return 0;
}

// ============================================================
// Initialization and registration
// ============================================================

void p8_init(tlsf_t tlsf) {
    p8_tlsf = tlsf;
    p8_mem = (uint8_t *)tlsf_malloc(tlsf, P8_MEM_SIZE);
    if (p8_mem)
        memset(p8_mem, 0, P8_MEM_SIZE);

    // Point gfx framebuffer at PICO-8 screen memory (0x6000-0x7FFF)
    // so that pset/pget and memcpy/peek/poke share the same buffer
    if (p8_mem)
        gfx_set_fb(p8_mem + P8_SCREEN);

    // Init palettes to identity
    for (int i = 0; i < 16; i++) {
        p8_draw_pal[i] = i;
        p8_display_pal[i] = i;
    }
    // Connect display palette to gfx_flip() for screen-level palette swaps
    gfx_set_display_pal(p8_display_pal);
    p8_palt_mask = 0x0001; // color 0 transparent
    p8_fill_pattern = 0;
    p8_fill_transparent = false;

    p8_start_time = get_absolute_time();
    p8_rng_state = 0x12345678;
    p8_cartdata_open = false;
}

void p8_reset_draw_state(void) {
    p8_draw_color = 6;
    p8_cursor_x = 0;
    p8_cursor_y = 0;
    p8_camera_x = 0;
    p8_camera_y = 0;
    p8_clip_x = 0;
    p8_clip_y = 0;
    p8_clip_w = 128;
    p8_clip_h = 128;
    for (int i = 0; i < 16; i++) {
        p8_draw_pal[i] = i;
        p8_display_pal[i] = i;
    }
    p8_palt_mask = 0x0001;
    p8_fill_pattern = 0;
    p8_fill_transparent = false;
    // Clear menuitem refs
    for (int i = 0; i < 5; i++)
        p8_menuitem_refs[i] = LUA_NOREF;
}

uint8_t *p8_get_memory(void) {
    return p8_mem;
}

void p8_register_api(lua_State *L) {
    // Phase 1: Core globals

    // Graphics
    lua_register(L, "cls", p8_cls);
    lua_register(L, "pset", p8_pset);
    lua_register(L, "pget", p8_pget);
    lua_register(L, "print", p8_print);   // Override Lua's print for PICO-8 screen drawing
    lua_register(L, "color", p8_color);
    lua_register(L, "cursor", p8_cursor);
    lua_register(L, "camera", p8_camera);
    lua_register(L, "clip", p8_clip);
    lua_register(L, "line", p8_line);
    lua_register(L, "rect", p8_rect);
    lua_register(L, "rectfill", p8_rectfill);
    lua_register(L, "circ", p8_circ);
    lua_register(L, "circfill", p8_circfill);
    lua_register(L, "oval", p8_oval);
    lua_register(L, "ovalfill", p8_ovalfill);
    lua_register(L, "spr", p8_spr);
    lua_register(L, "sspr", p8_sspr);
    lua_register(L, "flip", p8_flip);

    // Map
    lua_register(L, "map", p8_map);
    lua_register(L, "mget", p8_mget);
    lua_register(L, "mset", p8_mset);

    // Input
    lua_register(L, "btn", p8_btn);
    lua_register(L, "btnp", p8_btnp);

    // Math
    lua_register(L, "flr", p8_flr);
    lua_register(L, "ceil", p8_ceil);
    lua_register(L, "abs", p8_abs);
    lua_register(L, "max", p8_max);
    lua_register(L, "min", p8_min);
    lua_register(L, "mid", p8_mid);
    lua_register(L, "sin", p8_sin);
    lua_register(L, "cos", p8_cos);
    lua_register(L, "atan2", p8_atan2);
    lua_register(L, "sqrt", p8_sqrt);
    lua_register(L, "sgn", p8_sgn);
    lua_register(L, "rnd", p8_rnd);
    lua_register(L, "srand", p8_srand);

    // Table
    lua_register(L, "add", p8_add);
    lua_register(L, "del", p8_del);
    lua_register(L, "deli", p8_deli);
    lua_register(L, "count", p8_count);
    lua_register(L, "all", p8_all);
    lua_register(L, "foreach", p8_foreach);

    // String
    lua_register(L, "sub", p8_sub);
    lua_register(L, "chr", p8_chr);
    lua_register(L, "ord", p8_ord);
    lua_register(L, "tostr", p8_tostr);
    lua_register(L, "tonum", p8_tonum);
    lua_register(L, "split", p8_split);
    lua_register(L, "type", p8_type);

    // System
    lua_register(L, "time", p8_time);
    lua_register(L, "t", p8_time);
    lua_register(L, "stat", p8_stat);
    lua_register(L, "printh", p8_printh);
    lua_register(L, "stop", p8_stop);
    lua_register(L, "sfx", p8_sfx);
    lua_register(L, "music", p8_music);
    lua_register(L, "menuitem", p8_menuitem);
    lua_register(L, "extcmd", p8_extcmd);
    lua_register(L, "serial", p8_serial);
    lua_register(L, "reload", p8_reload);
    lua_register(L, "cstore", p8_cstore);

    // Drawing extras
    lua_register(L, "rrect", p8_rrect);
    lua_register(L, "rrectfill", p8_rrectfill);
    lua_register(L, "tline", p8_tline);

    // Coroutines
    lua_register(L, "cocreate", p8_cocreate);
    lua_register(L, "coresume", p8_coresume);
    lua_register(L, "costatus", p8_costatus);
    lua_register(L, "yield", p8_yield);

    // Bitwise
    lua_register(L, "band", p8_band);
    lua_register(L, "bor", p8_bor);
    lua_register(L, "bxor", p8_bxor);
    lua_register(L, "bnot", p8_bnot);
    lua_register(L, "shl", p8_shl);
    lua_register(L, "shr", p8_shr);
    lua_register(L, "lshr", p8_lshr);
    lua_register(L, "rotl", p8_rotl);
    lua_register(L, "rotr", p8_rotr);

    // Phase 2: Sprite sheet, flags, palette
    lua_register(L, "sget", p8_sget);
    lua_register(L, "sset", p8_sset);
    lua_register(L, "fget", p8_fget);
    lua_register(L, "fset", p8_fset);
    lua_register(L, "pal", p8_pal);
    lua_register(L, "palt", p8_palt);
    lua_register(L, "fillp", p8_fillp);

    // Phase 3: Memory access
    lua_register(L, "peek", p8_peek);
    lua_register(L, "poke", p8_poke);
    lua_register(L, "peek2", p8_peek2);
    lua_register(L, "poke2", p8_poke2);
    lua_register(L, "peek4", p8_peek4);
    lua_register(L, "poke4", p8_poke4);
    lua_register(L, "memcpy", p8_memcpy);
    lua_register(L, "memset", p8_memset);

    // Phase 4: Cartridge data
    lua_register(L, "cartdata", p8_cartdata_fn);
    lua_register(L, "dget", p8_dget);
    lua_register(L, "dset", p8_dset);

    // Predefined P8SCII glyph globals (preprocessor converts glyphs to _PG_XXXX)
    luaL_dostring(L,
        "_PG_2B05=0 "    /* ⬅️ */
        "_PG_27A1=1 "    /* ➡️ */
        "_PG_2B06=2 "    /* ⬆️ */
        "_PG_2B07=3 "    /* ⬇️ */
        "_PG_1F17E=4 "   /* 🅾️ */
        "_PG_274E=5 "    /* ❎ */
        "_PG_2591=23130.5 "  /* ░ fill pattern + transparency */
        "_PG_2592=42405.5 "  /* ▒ inverse fill + transparency */
        "_PG_2593=23130.0 "  /* ▓ fill pattern, no transparency */
    );

    // PICO-8 nil-safe pairs/ipairs: return empty iterator instead of error
    luaL_dostring(L,
        "do "
        "  local _pairs = pairs "
        "  function pairs(t) "
        "    if t == nil then return function() end end "
        "    return _pairs(t) "
        "  end "
        "  local _ipairs = ipairs "
        "  function ipairs(t) "
        "    if t == nil then return function() end, nil, 0 end "
        "    return _ipairs(t) "
        "  end "
        "end"
    );
}

// ============================================================
// Global snapshot / cleanup for cart switching
// ============================================================

// Registry key for the snapshot table
static const char *P8_GLOBALS_KEY = "p8_builtin_globals";

void p8_snapshot_globals(lua_State *L) {
    // Create a table that stores all current _G keys as keys with value=true
    lua_newtable(L);

    // Iterate _G
    lua_pushglobaltable(L);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        // Stack: snapshot_table, _G, key, value
        lua_pop(L, 1); // pop value, keep key
        // Copy key and set snapshot[key] = true
        lua_pushvalue(L, -1); // copy key
        lua_pushboolean(L, 1);
        lua_settable(L, -5); // snapshot[key] = true
    }
    lua_pop(L, 1); // pop _G

    // Store snapshot in registry
    lua_setfield(L, LUA_REGISTRYINDEX, P8_GLOBALS_KEY);
}

void p8_cleanup_globals(lua_State *L) {
    // Get snapshot table from registry
    lua_getfield(L, LUA_REGISTRYINDEX, P8_GLOBALS_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return; // no snapshot taken
    }
    int snap_idx = lua_gettop(L);

    lua_pushglobaltable(L);
    int g_idx = lua_gettop(L);

    // Collect keys to nil in a temp table (can't modify during lua_next)
    lua_newtable(L);
    int collect_idx = lua_gettop(L);
    int count = 0;

    lua_pushnil(L);
    while (lua_next(L, g_idx) != 0) {
        lua_pop(L, 1); // pop value, keep key
        lua_pushvalue(L, -1); // copy key
        lua_gettable(L, snap_idx);
        if (lua_isnil(L, -1)) {
            // Not in snapshot — collect for removal
            lua_pop(L, 1); // pop nil
            count++;
            lua_pushvalue(L, -1); // copy key
            lua_pushboolean(L, 1);
            lua_settable(L, collect_idx); // collect[key] = true
        } else {
            lua_pop(L, 1); // pop true
        }
    }

    // Now nil collected keys from _G
    if (count > 0) {
        lua_pushnil(L);
        while (lua_next(L, collect_idx) != 0) {
            lua_pop(L, 1); // pop value (true)
            lua_pushvalue(L, -1); // copy key
            lua_pushnil(L);
            lua_settable(L, g_idx); // _G[key] = nil
        }
    }

    lua_pop(L, 3); // pop collect, _G, snapshot
}
