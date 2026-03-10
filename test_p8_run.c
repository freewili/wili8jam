/*
 * test_p8_run.c — Host-side test harness for PICO-8 carts.
 *
 * Loads any .p8 file, parses data sections into simulated PICO-8 memory,
 * preprocesses the __lua__ section, and runs it through the Lua 5.4 VM
 * with stub API functions.
 *
 * Usage: test_p8_run <file.p8> [frames]
 *   frames: number of _update/_draw frames to run (default 10)
 *
 * Build (MinGW):
 *   gcc -o test_p8_run test_p8_run.c src/p8_preprocess.c tlsf/tlsf.c \
 *       lua-5.4.7/src/*.c -Isrc -Ilua-5.4.7/src -I. \
 *       -DLUA_32BITS=1 -lm
 *   (exclude lua.c and luac.c from the glob)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tlsf/tlsf.h"
#include "p8_preprocess.h"

/* ============================================================
 * Simulated PICO-8 memory (32KB)
 * ============================================================ */

static uint8_t p8_mem[0x8000];

/* ============================================================
 * .p8 section parsers (mirrors p8_cart.c logic)
 * ============================================================ */

static inline int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void parse_gfx(const char *data, size_t len) {
    int row = 0;
    const char *p = data;
    const char *end = data + len;
    while (p < end && row < 128) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        int col = 0;
        while (p < end && *p != '\n' && *p != '\r' && col < 128) {
            int v = hex_val(*p);
            if (v < 0) { p++; continue; }
            int addr = row * 64 + col / 2;
            if (col & 1)
                p8_mem[addr] = (p8_mem[addr] & 0x0F) | (v << 4);
            else
                p8_mem[addr] = (p8_mem[addr] & 0xF0) | v;
            col++;
            p++;
        }
        while (p < end && *p != '\n') p++;
        row++;
    }
}

static void parse_gff(const char *data, size_t len) {
    const char *p = data;
    const char *end = data + len;
    int idx = 0;
    while (p < end && idx < 256) {
        while (p < end && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
        if (p + 1 >= end) break;
        int hi = hex_val(*p);
        int lo = hex_val(*(p + 1));
        if (hi < 0 || lo < 0) { p++; continue; }
        p8_mem[0x3000 + idx] = (uint8_t)((hi << 4) | lo);
        idx++;
        p += 2;
    }
}

static void parse_map(const char *data, size_t len) {
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
            p8_mem[0x2000 + row * 128 + col] = (uint8_t)((hi << 4) | lo);
            col++;
            p += 2;
        }
        while (p < end && *p != '\n') p++;
        row++;
    }
}

static void parse_sfx(const char *data, size_t len) {
    const char *p = data;
    const char *end = data + len;
    int sfx_idx = 0;
    while (p < end && sfx_idx < 64) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        int addr = 0x3200 + sfx_idx * 68;
        uint8_t header[4] = {0, 0, 0, 0};
        for (int h = 0; h < 4 && p + 1 < end && *p != '\n'; h++) {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) header[h] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        p8_mem[addr + 64] = header[0];
        p8_mem[addr + 65] = header[1];
        p8_mem[addr + 66] = header[2];
        p8_mem[addr + 67] = header[3];
        for (int n = 0; n < 32 && p < end && *p != '\n' && *p != '\r'; n++) {
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
            uint16_t word = (uint16_t)(
                (custom << 0) | (pitch << 1) | (waveform << 7) |
                (volume << 10) | (effect << 13));
            p8_mem[addr + n * 2]     = (uint8_t)(word & 0xFF);
            p8_mem[addr + n * 2 + 1] = (uint8_t)(word >> 8);
        }
        while (p < end && *p != '\n') p++;
        sfx_idx++;
    }
}

static void parse_music(const char *data, size_t len) {
    const char *p = data;
    const char *end = data + len;
    int pat_idx = 0;
    while (p < end && pat_idx < 64) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        while (p < end && *p == ' ') p++;
        uint8_t flags = 0;
        if (p + 1 < end && *p != '\n') {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) flags = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        while (p < end && *p == ' ') p++;
        uint8_t ch_bytes[4] = {0x41, 0x41, 0x41, 0x41};
        for (int c = 0; c < 4 && p + 1 < end && *p != '\n' && *p != '\r'; c++) {
            int hi = hex_val(*p);
            int lo = hex_val(*(p + 1));
            if (hi >= 0 && lo >= 0) ch_bytes[c] = (uint8_t)((hi << 4) | lo);
            p += 2;
        }
        if (flags & 0x01) ch_bytes[0] |= 0x80;
        if (flags & 0x02) ch_bytes[1] |= 0x80;
        if (flags & 0x04) ch_bytes[2] |= 0x80;
        int addr = 0x3100 + pat_idx * 4;
        p8_mem[addr + 0] = ch_bytes[0];
        p8_mem[addr + 1] = ch_bytes[1];
        p8_mem[addr + 2] = ch_bytes[2];
        p8_mem[addr + 3] = ch_bytes[3];
        while (p < end && *p != '\n') p++;
        pat_idx++;
    }
}

/* ============================================================
 * Section finder
 * ============================================================ */

static const char *find_section(const char *data, size_t len, const char *header) {
    size_t hlen = strlen(header);
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        if ((size_t)(end - p) >= hlen && memcmp(p, header, hlen) == 0) {
            p += hlen;
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
            return p;
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return NULL;
}

static const char *find_section_end(const char *start, const char *data_end) {
    const char *p = start;
    while (p < data_end) {
        if (*p == '_' && p + 1 < data_end && *(p + 1) == '_') {
            if (p == start || *(p - 1) == '\n')
                return p;
        }
        p++;
    }
    return data_end;
}

/* ============================================================
 * Stub PICO-8 API functions for Lua
 * ============================================================ */

/* Frame counter for t() */
static int frame_count = 0;

/* Memory access */
static int l_peek(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int n = (int)luaL_optnumber(L, 2, 1);
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) {
        int a = addr + i;
        lua_pushnumber(L, (a >= 0 && a < 0x8000) ? p8_mem[a] : 0);
    }
    return n;
}

static int l_poke(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int nargs = lua_gettop(L);
    for (int i = 2; i <= nargs; i++) {
        int a = addr + (i - 2);
        if (a >= 0 && a < 0x8000)
            p8_mem[a] = (uint8_t)(int)luaL_checknumber(L, i);
    }
    return 0;
}

static int l_peek2(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    if (addr < 0 || addr + 1 >= 0x8000) { lua_pushnumber(L, 0); return 1; }
    int16_t val = (int16_t)(uint16_t)(p8_mem[addr] | (p8_mem[addr+1] << 8));
    lua_pushnumber(L, (lua_Number)val);
    return 1;
}

static int l_poke2(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int16_t val = (int16_t)luaL_checknumber(L, 2);
    if (addr >= 0 && addr + 1 < 0x8000) {
        p8_mem[addr]     = (uint8_t)(val & 0xFF);
        p8_mem[addr + 1] = (uint8_t)((val >> 8) & 0xFF);
    }
    return 0;
}

static int l_peek4(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    if (addr < 0 || addr + 3 >= 0x8000) { lua_pushnumber(L, 0); return 1; }
    int32_t val = (int32_t)(
        p8_mem[addr] | (p8_mem[addr+1] << 8) |
        (p8_mem[addr+2] << 16) | (p8_mem[addr+3] << 24));
    lua_pushnumber(L, (lua_Number)val / 65536.0);
    return 1;
}

static int l_poke4(lua_State *L) {
    int addr = (int)luaL_checknumber(L, 1);
    int32_t val = (int32_t)(luaL_checknumber(L, 2) * 65536.0);
    if (addr >= 0 && addr + 3 < 0x8000) {
        p8_mem[addr]     = (uint8_t)(val & 0xFF);
        p8_mem[addr + 1] = (uint8_t)((val >> 8) & 0xFF);
        p8_mem[addr + 2] = (uint8_t)((val >> 16) & 0xFF);
        p8_mem[addr + 3] = (uint8_t)((val >> 24) & 0xFF);
    }
    return 0;
}

static int l_memcpy(lua_State *L) {
    int dest = (int)luaL_checknumber(L, 1);
    int src  = (int)luaL_checknumber(L, 2);
    int len  = (int)luaL_checknumber(L, 3);
    if (dest >= 0 && src >= 0 && len > 0 &&
        dest + len <= 0x8000 && src + len <= 0x8000)
        memmove(&p8_mem[dest], &p8_mem[src], len);
    return 0;
}

static int l_memset(lua_State *L) {
    int dest = (int)luaL_checknumber(L, 1);
    int val  = (int)luaL_checknumber(L, 2);
    int len  = (int)luaL_checknumber(L, 3);
    if (dest >= 0 && len > 0 && dest + len <= 0x8000)
        memset(&p8_mem[dest], val & 0xFF, len);
    return 0;
}

/* Table functions */
static int l_add(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int idx = (int)luaL_optnumber(L, 3, 0);
    int n = (int)luaL_len(L, 1);
    if (idx <= 0 || idx > n + 1) {
        /* Append */
        lua_pushvalue(L, 2);
        lua_rawseti(L, 1, n + 1);
    } else {
        /* Insert at idx */
        for (int i = n; i >= idx; i--) {
            lua_rawgeti(L, 1, i);
            lua_rawseti(L, 1, i + 1);
        }
        lua_pushvalue(L, 2);
        lua_rawseti(L, 1, idx);
    }
    lua_pushvalue(L, 2);
    return 1;
}

static int l_del(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    /* Find and remove first occurrence of val */
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        if (lua_rawequal(L, -1, 2)) {
            lua_pop(L, 1);
            /* Shift down */
            for (int j = i; j < n; j++) {
                lua_rawgeti(L, 1, j + 1);
                lua_rawseti(L, 1, j);
            }
            lua_pushnil(L);
            lua_rawseti(L, 1, n);
            lua_pushvalue(L, 2);
            return 1;
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int l_deli(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int n = (int)luaL_len(L, 1);
    int i = (int)luaL_optnumber(L, 2, n);
    if (i < 1 || i > n) return 0;
    lua_rawgeti(L, 1, i);
    for (int j = i; j < n; j++) {
        lua_rawgeti(L, 1, j + 1);
        lua_rawseti(L, 1, j);
    }
    lua_pushnil(L);
    lua_rawseti(L, 1, n);
    return 1;
}

static int l_count(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (lua_gettop(L) >= 2) {
        /* Count occurrences of val */
        int n = (int)luaL_len(L, 1);
        int c = 0;
        for (int i = 1; i <= n; i++) {
            lua_rawgeti(L, 1, i);
            if (lua_rawequal(L, -1, 2)) c++;
            lua_pop(L, 1);
        }
        lua_pushnumber(L, c);
    } else {
        lua_pushnumber(L, (lua_Number)luaL_len(L, 1));
    }
    return 1;
}

static int l_all_iter(lua_State *L) {
    int i = (int)lua_tointeger(L, lua_upvalueindex(2));
    int n = (int)luaL_len(L, lua_upvalueindex(1));
    i++;
    if (i > n) return 0;
    lua_pushinteger(L, i);
    lua_replace(L, lua_upvalueindex(2));
    lua_rawgeti(L, lua_upvalueindex(1), i);
    return 1;
}

static int l_all(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, l_all_iter, 2);
    return 1;
}

static int l_foreach(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    int n = (int)luaL_len(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(L, 2);
        lua_rawgeti(L, 1, i);
        lua_call(L, 1, 0);
    }
    return 0;
}

/* Math functions (PICO-8 conventions) */
static int l_flr(lua_State *L) {
    lua_pushnumber(L, floor(luaL_checknumber(L, 1)));
    return 1;
}

static int l_ceil(lua_State *L) {
    lua_pushnumber(L, ceil(luaL_checknumber(L, 1)));
    return 1;
}

static int l_abs(lua_State *L) {
    lua_pushnumber(L, fabs(luaL_checknumber(L, 1)));
    return 1;
}

static int l_sgn(lua_State *L) {
    lua_Number n = luaL_checknumber(L, 1);
    lua_pushnumber(L, n < 0 ? -1 : 1);
    return 1;
}

static int l_sqrt(lua_State *L) {
    lua_pushnumber(L, sqrt(luaL_checknumber(L, 1)));
    return 1;
}

static int l_min(lua_State *L) {
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);
    lua_pushnumber(L, a < b ? a : b);
    return 1;
}

static int l_max(lua_State *L) {
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);
    lua_pushnumber(L, a > b ? a : b);
    return 1;
}

static int l_mid(lua_State *L) {
    lua_Number a = luaL_checknumber(L, 1);
    lua_Number b = luaL_checknumber(L, 2);
    lua_Number c = luaL_checknumber(L, 3);
    lua_Number lo = a < b ? a : b;
    lua_Number hi = a > b ? a : b;
    lua_pushnumber(L, c < lo ? lo : (c > hi ? hi : c));
    return 1;
}

/* PICO-8 sin/cos: use turns (0..1 = full rotation), sin is inverted */
static int l_sin(lua_State *L) {
    lua_Number n = luaL_checknumber(L, 1);
    lua_pushnumber(L, -sin(n * 2.0 * 3.14159265358979323846));
    return 1;
}

static int l_cos(lua_State *L) {
    lua_Number n = luaL_checknumber(L, 1);
    lua_pushnumber(L, cos(n * 2.0 * 3.14159265358979323846));
    return 1;
}

static int l_atan2(lua_State *L) {
    lua_Number dx = luaL_checknumber(L, 1);
    lua_Number dy = luaL_checknumber(L, 2);
    lua_pushnumber(L, atan2(-dy, dx) / (2.0 * 3.14159265358979323846));
    return 1;
}

static int l_rnd(lua_State *L) {
    lua_Number x = luaL_optnumber(L, 1, 1);
    if (lua_istable(L, 1)) {
        int n = (int)luaL_len(L, 1);
        if (n == 0) { lua_pushnil(L); return 1; }
        lua_rawgeti(L, 1, (rand() % n) + 1);
        return 1;
    }
    lua_pushnumber(L, ((lua_Number)(rand() % 32768) / 32768.0) * x);
    return 1;
}

static int l_srand(lua_State *L) {
    srand((unsigned)(luaL_checknumber(L, 1) * 65536.0));
    return 0;
}

/* Bitwise ops */
static int l_band(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int32_t b = (int32_t)(luaL_checknumber(L, 2) * 65536.0);
    lua_pushnumber(L, (lua_Number)(a & b) / 65536.0);
    return 1;
}

static int l_bor(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int32_t b = (int32_t)(luaL_checknumber(L, 2) * 65536.0);
    lua_pushnumber(L, (lua_Number)(a | b) / 65536.0);
    return 1;
}

static int l_bxor(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int32_t b = (int32_t)(luaL_checknumber(L, 2) * 65536.0);
    lua_pushnumber(L, (lua_Number)(a ^ b) / 65536.0);
    return 1;
}

static int l_bnot(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    lua_pushnumber(L, (lua_Number)(~a) / 65536.0);
    return 1;
}

static int l_shl(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int n = (int)luaL_checknumber(L, 2);
    lua_pushnumber(L, (lua_Number)(n < 32 ? a << n : 0) / 65536.0);
    return 1;
}

static int l_shr(lua_State *L) {
    int32_t a = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int n = (int)luaL_checknumber(L, 2);
    int32_t result = (n < 32) ? (int32_t)((uint32_t)a >> n) : 0;
    lua_pushnumber(L, (lua_Number)result / 65536.0);
    return 1;
}

static int l_lshr(lua_State *L) { return l_shr(L); }

static int l_rotl(lua_State *L) {
    uint32_t a = (uint32_t)(int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int n = (int)luaL_checknumber(L, 2) & 31;
    uint32_t result = (a << n) | (a >> (32 - n));
    lua_pushnumber(L, (lua_Number)(int32_t)result / 65536.0);
    return 1;
}

static int l_rotr(lua_State *L) {
    uint32_t a = (uint32_t)(int32_t)(luaL_checknumber(L, 1) * 65536.0);
    int n = (int)luaL_checknumber(L, 2) & 31;
    uint32_t result = (a >> n) | (a << (32 - n));
    lua_pushnumber(L, (lua_Number)(int32_t)result / 65536.0);
    return 1;
}

/* String functions */
static int l_sub(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    int i = (int)luaL_checknumber(L, 2);
    int j = (int)luaL_optnumber(L, 3, (lua_Number)len);
    /* PICO-8 is 1-based */
    if (i < 1) i = 1;
    if (j > (int)len) j = (int)len;
    if (i > j) { lua_pushliteral(L, ""); return 1; }
    lua_pushlstring(L, s + i - 1, j - i + 1);
    return 1;
}

static int l_tostr(lua_State *L) {
    if (lua_isnoneornil(L, 1)) {
        lua_pushliteral(L, "");
        return 1;
    }
    int flags = (int)luaL_optnumber(L, 2, 0);
    if (flags & 1) {
        /* Hex format */
        int32_t val = (int32_t)(luaL_checknumber(L, 1) * 65536.0);
        char buf[16];
        snprintf(buf, sizeof(buf), "0x%04x.%04x",
                 (uint16_t)(val >> 16), (uint16_t)(val & 0xFFFF));
        lua_pushstring(L, buf);
    } else {
        luaL_tolstring(L, 1, NULL);
    }
    return 1;
}

static int l_tonum(lua_State *L) {
    if (lua_isnumber(L, 1)) {
        lua_pushnumber(L, lua_tonumber(L, 1));
    } else if (lua_isstring(L, 1)) {
        const char *s = lua_tostring(L, 1);
        char *endp;
        double v = strtod(s, &endp);
        if (endp != s)
            lua_pushnumber(L, v);
        else
            lua_pushnil(L);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static int l_chr(lua_State *L) {
    int c = (int)luaL_checknumber(L, 1);
    char buf[2] = { (char)c, 0 };
    lua_pushlstring(L, buf, 1);
    return 1;
}

static int l_ord(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    int i = (int)luaL_optnumber(L, 2, 1);
    if (i < 1 || i > (int)len) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (unsigned char)s[i - 1]);
    return 1;
}

static int l_split(lua_State *L) {
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    const char *sep = luaL_optstring(L, 2, ",");
    size_t sep_len = strlen(sep);
    lua_newtable(L);
    int idx = 1;
    const char *p = s;
    const char *end = s + len;
    while (p <= end) {
        const char *next = NULL;
        if (sep_len == 0) {
            /* Split each character */
            if (p < end) {
                lua_pushlstring(L, p, 1);
                lua_rawseti(L, -2, idx++);
                p++;
                continue;
            }
            break;
        }
        next = strstr(p, sep);
        if (!next) next = end;
        lua_pushlstring(L, p, next - p);
        lua_rawseti(L, -2, idx++);
        p = next + sep_len;
    }
    return 1;
}

/* Graphics stubs (no-ops for headless testing) */
static int l_noop(lua_State *L) { (void)L; return 0; }

static int l_noop_return0(lua_State *L) {
    lua_pushnumber(L, 0);
    return 1;
}

/* Input stubs */
static int l_btn(lua_State *L) {
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

static int l_btnp(lua_State *L) {
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

/* Time */
static int l_time(lua_State *L) {
    (void)L;
    lua_pushnumber(L, (lua_Number)frame_count / 30.0);
    return 1;
}

/* stat() */
static int l_stat(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    switch (n) {
        case 0: lua_pushnumber(L, 64); break;    /* memory usage (fake) */
        case 1: lua_pushnumber(L, 0.5); break;   /* CPU (fake) */
        case 4: lua_pushliteral(L, ""); break;    /* clipboard */
        case 6: lua_pushnumber(L, 0); break;      /* mouse x */
        case 7: lua_pushnumber(L, 0); break;      /* mouse y */
        case 34: lua_pushnumber(L, 0); break;     /* mouse buttons */
        case 36: lua_pushnumber(L, 0); break;     /* mouse wheel */
        default: lua_pushnumber(L, 0); break;
    }
    return 1;
}

/* Map access */
static int l_mget(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    if (x < 0 || x >= 128 || y < 0 || y >= 64) { lua_pushnumber(L, 0); return 1; }
    int addr;
    if (y < 32)
        addr = 0x2000 + y * 128 + x;
    else
        addr = (y - 32) * 128 + x; /* shared gfx/map space */
    lua_pushnumber(L, p8_mem[addr]);
    return 1;
}

static int l_mset(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int v = (int)luaL_checknumber(L, 3);
    if (x < 0 || x >= 128 || y < 0 || y >= 64) return 0;
    int addr;
    if (y < 32)
        addr = 0x2000 + y * 128 + x;
    else
        addr = (y - 32) * 128 + x;
    p8_mem[addr] = (uint8_t)v;
    return 0;
}

/* Sprite flags */
static int l_fget(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    if (n < 0 || n >= 256) { lua_pushnumber(L, 0); return 1; }
    uint8_t flags = p8_mem[0x3000 + n];
    if (lua_gettop(L) >= 2) {
        int f = (int)luaL_checknumber(L, 2);
        lua_pushboolean(L, (flags >> f) & 1);
    } else {
        lua_pushnumber(L, flags);
    }
    return 1;
}

static int l_fset(lua_State *L) {
    int n = (int)luaL_checknumber(L, 1);
    if (n < 0 || n >= 256) return 0;
    if (lua_gettop(L) >= 3) {
        int f = (int)luaL_checknumber(L, 2);
        int v = lua_toboolean(L, 3);
        if (v) p8_mem[0x3000 + n] |= (1 << f);
        else   p8_mem[0x3000 + n] &= ~(1 << f);
    } else {
        p8_mem[0x3000 + n] = (uint8_t)(int)luaL_checknumber(L, 2);
    }
    return 0;
}

/* Sprite pixel access */
static int l_sget(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    if (x < 0 || x >= 128 || y < 0 || y >= 128) { lua_pushnumber(L, 0); return 1; }
    int addr = y * 64 + x / 2;
    int nib = (x & 1) ? (p8_mem[addr] >> 4) : (p8_mem[addr] & 0x0F);
    lua_pushnumber(L, nib);
    return 1;
}

static int l_sset(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int c = (int)luaL_checknumber(L, 3) & 0x0F;
    if (x < 0 || x >= 128 || y < 0 || y >= 128) return 0;
    int addr = y * 64 + x / 2;
    if (x & 1)
        p8_mem[addr] = (p8_mem[addr] & 0x0F) | (c << 4);
    else
        p8_mem[addr] = (p8_mem[addr] & 0xF0) | c;
    return 0;
}

/* Screen pixel access (screen at 0x6000) */
static int l_pset(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    int c = (int)luaL_optnumber(L, 3, 0) & 0x0F;
    if (x < 0 || x >= 128 || y < 0 || y >= 128) return 0;
    int addr = 0x6000 + y * 64 + x / 2;
    if (x & 1)
        p8_mem[addr] = (p8_mem[addr] & 0x0F) | (c << 4);
    else
        p8_mem[addr] = (p8_mem[addr] & 0xF0) | c;
    return 0;
}

static int l_pget(lua_State *L) {
    int x = (int)luaL_checknumber(L, 1);
    int y = (int)luaL_checknumber(L, 2);
    if (x < 0 || x >= 128 || y < 0 || y >= 128) { lua_pushnumber(L, 0); return 1; }
    int addr = 0x6000 + y * 64 + x / 2;
    int nib = (x & 1) ? (p8_mem[addr] >> 4) : (p8_mem[addr] & 0x0F);
    lua_pushnumber(L, nib);
    return 1;
}

/* cartdata / dget / dset stubs */
static lua_Number cart_data[64];

static int l_cartdata(lua_State *L) { (void)L; return 0; }

static int l_dget(lua_State *L) {
    int idx = (int)luaL_checknumber(L, 1);
    lua_pushnumber(L, (idx >= 0 && idx < 64) ? cart_data[idx] : 0);
    return 1;
}

static int l_dset(lua_State *L) {
    int idx = (int)luaL_checknumber(L, 1);
    lua_Number val = luaL_checknumber(L, 2);
    if (idx >= 0 && idx < 64) cart_data[idx] = val;
    return 0;
}

/* menuitem stub */
static int l_menuitem(lua_State *L) { (void)L; return 0; }

/* reload stub */
static int l_reload(lua_State *L) { (void)L; return 0; }

/* printh */
static int l_printh(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    printf("[printh] %s\n", s);
    return 0;
}

/* ============================================================
 * Register all stubs
 * ============================================================ */

static void register_p8_api(lua_State *L) {
    /* Memory */
    lua_register(L, "peek", l_peek);
    lua_register(L, "poke", l_poke);
    lua_register(L, "peek2", l_peek2);
    lua_register(L, "poke2", l_poke2);
    lua_register(L, "peek4", l_peek4);
    lua_register(L, "poke4", l_poke4);
    lua_register(L, "memcpy", l_memcpy);
    lua_register(L, "memset", l_memset);

    /* Table */
    lua_register(L, "add", l_add);
    lua_register(L, "del", l_del);
    lua_register(L, "deli", l_deli);
    lua_register(L, "count", l_count);
    lua_register(L, "all", l_all);
    lua_register(L, "foreach", l_foreach);

    /* Math */
    lua_register(L, "flr", l_flr);
    lua_register(L, "ceil", l_ceil);
    lua_register(L, "abs", l_abs);
    lua_register(L, "sgn", l_sgn);
    lua_register(L, "sqrt", l_sqrt);
    lua_register(L, "min", l_min);
    lua_register(L, "max", l_max);
    lua_register(L, "mid", l_mid);
    lua_register(L, "sin", l_sin);
    lua_register(L, "cos", l_cos);
    lua_register(L, "atan2", l_atan2);
    lua_register(L, "rnd", l_rnd);
    lua_register(L, "srand", l_srand);

    /* Bitwise */
    lua_register(L, "band", l_band);
    lua_register(L, "bor", l_bor);
    lua_register(L, "bxor", l_bxor);
    lua_register(L, "bnot", l_bnot);
    lua_register(L, "shl", l_shl);
    lua_register(L, "shr", l_shr);
    lua_register(L, "lshr", l_lshr);
    lua_register(L, "rotl", l_rotl);
    lua_register(L, "rotr", l_rotr);

    /* String */
    lua_register(L, "sub", l_sub);
    lua_register(L, "tostr", l_tostr);
    lua_register(L, "tonum", l_tonum);
    lua_register(L, "chr", l_chr);
    lua_register(L, "ord", l_ord);
    lua_register(L, "split", l_split);

    /* Input */
    lua_register(L, "btn", l_btn);
    lua_register(L, "btnp", l_btnp);

    /* Time */
    lua_register(L, "time", l_time);
    lua_register(L, "t", l_time);

    /* System */
    lua_register(L, "stat", l_stat);
    lua_register(L, "printh", l_printh);

    /* Map */
    lua_register(L, "mget", l_mget);
    lua_register(L, "mset", l_mset);
    lua_register(L, "fget", l_fget);
    lua_register(L, "fset", l_fset);

    /* Sprite memory */
    lua_register(L, "sget", l_sget);
    lua_register(L, "sset", l_sset);

    /* Screen pixel */
    lua_register(L, "pset", l_pset);
    lua_register(L, "pget", l_pget);

    /* Cart data / lifecycle */
    lua_register(L, "cartdata", l_cartdata);
    lua_register(L, "dget", l_dget);
    lua_register(L, "dset", l_dset);
    lua_register(L, "menuitem", l_menuitem);
    lua_register(L, "reload", l_reload);
    lua_register(L, "reset", l_noop);
    lua_register(L, "stop", l_noop);
    lua_register(L, "resume", l_noop);
    lua_register(L, "run", l_noop);

    /* Graphics stubs (drawing is a no-op in headless mode) */
    lua_register(L, "cls", l_noop);
    lua_register(L, "spr", l_noop);
    lua_register(L, "sspr", l_noop);
    lua_register(L, "map", l_noop);
    lua_register(L, "line", l_noop);
    lua_register(L, "rect", l_noop);
    lua_register(L, "rectfill", l_noop);
    lua_register(L, "circ", l_noop);
    lua_register(L, "circfill", l_noop);
    lua_register(L, "oval", l_noop);
    lua_register(L, "ovalfill", l_noop);
    lua_register(L, "pal", l_noop);
    lua_register(L, "palt", l_noop);
    lua_register(L, "fillp", l_noop);
    lua_register(L, "camera", l_noop);
    lua_register(L, "clip", l_noop);
    lua_register(L, "color", l_noop);
    lua_register(L, "cursor", l_noop);
    lua_register(L, "flip", l_noop);
    lua_register(L, "sfx", l_noop);
    lua_register(L, "music", l_noop);
    lua_register(L, "cstore", l_noop);
    lua_register(L, "extcmd", l_noop);

    /* PICO-8 pairs/ipairs are nil-safe (iterate nothing on nil) */
    luaL_dostring(L,
        "local _pairs = pairs\n"
        "function pairs(t) if t == nil then return function() end end return _pairs(t) end\n"
        "local _ipairs = ipairs\n"
        "function ipairs(t) if t == nil then return function() end, nil, 0 end return _ipairs(t) end\n"
    );

    /* P8SCII glyph constants (used by preprocessor) */
    lua_pushnumber(L, 0); lua_setglobal(L, "_PG_2B05"); /* ⬅️ left */
    lua_pushnumber(L, 1); lua_setglobal(L, "_PG_27A1"); /* ➡️ right */
    lua_pushnumber(L, 2); lua_setglobal(L, "_PG_2B06"); /* ⬆️ up */
    lua_pushnumber(L, 3); lua_setglobal(L, "_PG_2B07"); /* ⬇️ down */
    lua_pushnumber(L, 4); lua_setglobal(L, "_PG_1F17E"); /* 🅾️ O */
    lua_pushnumber(L, 5); lua_setglobal(L, "_PG_274E"); /* ❎ X */
}

/* ============================================================
 * Main
 * ============================================================ */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file.p8> [frames]\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int num_frames = (argc >= 3) ? atoi(argv[2]) : 10;

    /* Read file */
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *file = (char *)malloc(sz + 1);
    fread(file, 1, sz, f);
    file[sz] = 0;
    fclose(f);

    /* Init TLSF for preprocessor */
    static char heap[4 * 1024 * 1024];
    tlsf_t tlsf = tlsf_create_with_pool(heap, sizeof(heap));
    if (!tlsf) { fprintf(stderr, "TLSF init failed\n"); free(file); return 1; }
    p8_preprocess_init(tlsf);

    /* Clear memory and parse data sections */
    memset(p8_mem, 0, sizeof(p8_mem));
    const char *data_end = file + sz;

    const char *sec;
    const char *sec_end;

    sec = find_section(file, sz, "__gfx__");
    if (sec) { sec_end = find_section_end(sec, data_end); parse_gfx(sec, sec_end - sec); }

    sec = find_section(file, sz, "__gff__");
    if (sec) { sec_end = find_section_end(sec, data_end); parse_gff(sec, sec_end - sec); }

    sec = find_section(file, sz, "__map__");
    if (sec) { sec_end = find_section_end(sec, data_end); parse_map(sec, sec_end - sec); }

    sec = find_section(file, sz, "__sfx__");
    if (sec) { sec_end = find_section_end(sec, data_end); parse_sfx(sec, sec_end - sec); }

    sec = find_section(file, sz, "__music__");
    if (sec) { sec_end = find_section_end(sec, data_end); parse_music(sec, sec_end - sec); }

    /* Extract __lua__ section */
    sec = find_section(file, sz, "__lua__");
    if (!sec) { fprintf(stderr, "No __lua__ section in %s\n", path); free(file); return 1; }
    sec_end = find_section_end(sec, data_end);
    size_t lua_len = sec_end - sec;

    /* Preprocess */
    size_t pp_len;
    char *pp_code = p8_preprocess(sec, lua_len, &pp_len);
    if (!pp_code) {
        fprintf(stderr, "Preprocess failed for %s\n", path);
        free(file);
        return 1;
    }
    free(file);

    printf("=== %s ===\n", path);
    printf("Lua code: %zu bytes (preprocessed: %zu bytes)\n", lua_len, pp_len);

    /* Create Lua state and register API */
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    register_p8_api(L);

    /* Load and execute (defines _init/_update/_draw) */
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);

    int status = luaL_loadbuffer(L, pp_code, pp_len, chunkname);
    if (status != LUA_OK) {
        fprintf(stderr, "LOAD ERROR: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        fprintf(stderr, "EXEC ERROR: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }

    printf("Top-level code: OK\n");

    /* Call _init() if defined */
    lua_getglobal(L, "_init");
    if (lua_isfunction(L, -1)) {
        status = lua_pcall(L, 0, 0, 0);
        if (status != LUA_OK) {
            fprintf(stderr, "_init ERROR: %s\n", lua_tostring(L, -1));
            lua_close(L);
            return 1;
        }
        printf("_init(): OK\n");
    } else {
        lua_pop(L, 1);
    }

    /* Determine update function */
    const char *update_fn = "_update";
    lua_getglobal(L, "_update60");
    if (lua_isfunction(L, -1)) update_fn = "_update60";
    lua_pop(L, 1);

    /* Run frames */
    int errors = 0;
    for (int i = 0; i < num_frames; i++) {
        frame_count = i;

        /* _update / _update60 */
        lua_getglobal(L, update_fn);
        if (lua_isfunction(L, -1)) {
            status = lua_pcall(L, 0, 0, 0);
            if (status != LUA_OK) {
                fprintf(stderr, "%s frame %d ERROR: %s\n",
                        update_fn, i, lua_tostring(L, -1));
                lua_pop(L, 1);
                errors++;
                break;
            }
        } else {
            lua_pop(L, 1);
        }

        /* _draw */
        lua_getglobal(L, "_draw");
        if (lua_isfunction(L, -1)) {
            status = lua_pcall(L, 0, 0, 0);
            if (status != LUA_OK) {
                fprintf(stderr, "_draw frame %d ERROR: %s\n",
                        i, lua_tostring(L, -1));
                lua_pop(L, 1);
                errors++;
                break;
            }
        } else {
            lua_pop(L, 1);
        }
    }

    if (errors == 0) {
        printf("Ran %d frames: OK\n", num_frames);
    }

    printf("RESULT: %s\n", errors == 0 ? "PASS" : "FAIL");

    lua_close(L);
    return errors == 0 ? 0 : 1;
}
