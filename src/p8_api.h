#ifndef P8_API_H
#define P8_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "lua.h"
#include "tlsf/tlsf.h"

// Initialize the PICO-8 virtual memory (32KB in PSRAM) and API state.
// Must be called after TLSF init and before p8_register_api().
void p8_init(tlsf_t tlsf);

// Register all PICO-8 global functions into the Lua state.
void p8_register_api(lua_State *L);

// Access to PICO-8 virtual memory (32KB: 0x0000-0x7FFF)
uint8_t *p8_get_memory(void);

// Set whether flip() is managed by the game loop (skip sleep/input in flip).
void p8_set_gameloop_mode(bool enabled);

// Set CPU usage fraction (0.0-1.0+) for stat(1).
void p8_set_cpu_usage(float usage);

// Set target FPS (30 or 60) for stat(7)/stat(8).
void p8_set_target_fps(int fps);

// Reset PICO-8 draw state (palette, camera, clip, cursor, color, fill pattern).
void p8_reset_draw_state(void);

// Re-register the PICO-8 screen-drawing print function.
// Call before game loop to override console_print.
void p8_register_print(lua_State *L);

// Snapshot current _G keys as "built-in" globals.
// Call once after all API registration is complete.
void p8_snapshot_globals(lua_State *L);

// Nil all user-defined globals (anything not in the snapshot).
// Call before loading a new cart to prevent global leakage.
void p8_cleanup_globals(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // P8_API_H
