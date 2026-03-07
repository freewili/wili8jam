#ifndef GFX_H
#define GFX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

#define GFX_WIDTH  128
#define GFX_HEIGHT 128

// Initialize graphics subsystem (clears framebuffer, sets up palette).
// Call before dvi_init().
void gfx_init(void);

// Returns pointer to the RGB565 DVI framebuffer (for passing to dvi_init).
uint16_t *gfx_get_dvi_buffer(void);

// Drawing API (all coordinates clipped to 0..127, colors masked to 0..15)
void gfx_cls(int color);
void gfx_pset(int x, int y, int c);
int  gfx_pget(int x, int y);
void gfx_rect(int x0, int y0, int x1, int y1, int c);
void gfx_rectfill(int x0, int y0, int x1, int y1, int c);
void gfx_line(int x0, int y0, int x1, int y1, int c);
void gfx_circ(int cx, int cy, int r, int c);
void gfx_circfill(int cx, int cy, int r, int c);
void gfx_print(const char *str, int x, int y, int c);
void gfx_print_w(const char *str, int x, int y, int c, int char_w);
int  gfx_text_width(const char *str, int char_w);
void gfx_flip(void);

// Redirect the 4-bit framebuffer to an external buffer (e.g., PICO-8 screen memory).
// Pass NULL to restore the default internal buffer.
void gfx_set_fb(uint8_t *buf);
uint8_t *gfx_get_fb(void);

// Set display palette for gfx_flip(). Maps framebuffer color index to display color.
// Pass NULL to disable (identity mapping). The pointer must remain valid.
void gfx_set_display_pal(const uint8_t *pal);

// Lua library opener
int luaopen_gfx(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // GFX_H
