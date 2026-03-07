#ifndef P8_CONSOLE_H
#define P8_CONSOLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// Initialize the on-screen console.
void p8_console_init(void);

// Print a string to the on-screen console (with auto-scrolling).
void p8_console_print(const char *str);

// Print formatted string to console.
void p8_console_printf(const char *fmt, ...);

// Clear the console.
void p8_console_clear(void);

// Set console background color (0-15).
void p8_console_set_bg(int color);

// Draw the console contents onto the framebuffer.
// Call this before gfx_flip() when in REPL/boot mode.
void p8_console_draw(void);

// Register the PICO-8-style print global that draws to the on-screen console
// when called with 1 argument (like Lua's print), and to the screen when
// called with coordinates (like PICO-8's print).
void p8_console_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // P8_CONSOLE_H
