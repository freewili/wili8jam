/*
 * On-screen console for wili8jam
 *
 * Ring buffer of text lines rendered on the 128x128 DVI framebuffer.
 * Provides a PICO-8-like boot/REPL experience without serial connection.
 */

#include "p8_console.h"
#include "gfx.h"
#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// Console dimensions (using 4x6 font)
#define CON_COLS 32    // 128 / 4 = 32 chars per line (4px = 3px glyph + 1px gap)
#define CON_ROWS 20    // 120px / 6 = 20 lines, leaving room for input line
#define CON_BUF_ROWS 64 // ring buffer size (keep scrollback)

static char con_buf[CON_BUF_ROWS][CON_COLS + 1];
static int con_head = 0;    // next row to write
static int con_count = 0;   // total rows written (for scroll position)
static int con_cursor_x = 0; // cursor X within current line

// Console colors
static int con_bg_color = 0;   // black (can be changed by cls)
#define CON_FG_COLOR 6   // light grey
#define CON_PROMPT_COLOR 12 // blue

void p8_console_init(void) {
    memset(con_buf, 0, sizeof(con_buf));
    con_head = 0;
    con_count = 0;
    con_cursor_x = 0;
}

static void con_newline(void) {
    con_head = (con_head + 1) % CON_BUF_ROWS;
    con_count++;
    con_cursor_x = 0;
    memset(con_buf[con_head], 0, CON_COLS + 1);
}

void p8_console_print(const char *str) {
    if (!str) return;

    while (*str) {
        if (*str == '\n') {
            con_newline();
            str++;
            continue;
        }
        if (con_cursor_x >= CON_COLS) {
            con_newline();
        }
        con_buf[con_head][con_cursor_x++] = *str++;
        con_buf[con_head][con_cursor_x] = '\0';
    }
}

void p8_console_printf(const char *fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    p8_console_print(tmp);
}

void p8_console_clear(void) {
    memset(con_buf, 0, sizeof(con_buf));
    con_head = 0;
    con_count = 0;
    con_cursor_x = 0;
}

void p8_console_set_bg(int color) {
    con_bg_color = color & 0xF;
}

void p8_console_draw(void) {
    gfx_cls(con_bg_color);

    // Calculate which rows to show (most recent CON_ROWS lines)
    int start;
    int visible = con_count + 1; // +1 for the current partial line
    if (visible > CON_ROWS) visible = CON_ROWS;

    for (int i = 0; i < visible; i++) {
        // Calculate ring buffer index
        int row_idx = con_head - (visible - 1 - i);
        if (row_idx < 0) row_idx += CON_BUF_ROWS;
        row_idx %= CON_BUF_ROWS;

        int y = i * 6;
        if (con_buf[row_idx][0] != '\0') {
            gfx_print_w(con_buf[row_idx], 0, y, CON_FG_COLOR, 4);
        }
    }
}

// Smart print: if called with (str) or (str, color) → console output.
// If called with (str, x, y, [c]) → screen draw (PICO-8 style).
// This replaces the p8_print from p8_api.c in REPL mode.
static int console_print(lua_State *L) {
    int nargs = lua_gettop(L);

    // If 3+ args and args 2,3 are numbers → PICO-8 screen print
    if (nargs >= 3 && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
        // Delegate to PICO-8 screen print (gfx_print)
        const char *str = luaL_tolstring(L, 1, NULL);
        lua_pop(L, 1);
        if (!str) str = "";
        int x = (int)lua_tonumber(L, 2);
        int y = (int)lua_tonumber(L, 3);
        int c = nargs >= 4 ? (int)lua_tonumber(L, 4) : 6;
        gfx_print(str, x, y, c & 0xF);
        return 0;
    }

    // Console mode: print all args separated by tabs (like standard Lua print)
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) p8_console_print("\t");
        const char *s = luaL_tolstring(L, i, NULL);
        if (s) p8_console_print(s);
        lua_pop(L, 1);
    }
    p8_console_print("\n");

    // Mirror to serial (USB CDC)
    for (int i = 1; i <= n; i++) {
        if (i > 1) printf("\t");
        const char *s = luaL_tolstring(L, i, NULL);
        if (s) printf("%s", s);
        lua_pop(L, 1);
    }
    printf("\n");

    return 0;
}

void p8_console_register(lua_State *L) {
    lua_register(L, "print", console_print);
}
