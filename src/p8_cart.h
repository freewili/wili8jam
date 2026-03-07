#ifndef P8_CART_H
#define P8_CART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"
#include "tlsf/tlsf.h"

// Initialize the cart loader. Call once after p8_init().
void p8_cart_init(tlsf_t tlsf);

// Parse a .p8 file from a buffer. Loads __gfx__, __gff__, __map__, __sfx__,
// __music__ sections into PICO-8 virtual memory. Returns the __lua__ section
// as a newly allocated string (caller must tlsf_free). Sets *lua_len.
// Returns NULL if no __lua__ section found.
char *p8_cart_parse(const char *data, size_t data_len, size_t *lua_len);

// Load a .p8 cartridge from SD card without executing.
// Parses data sections into vram, sets current cart path, loads code into editor.
// Does NOT preprocess/execute lua or enter game loop.
// Returns 0 on success, nonzero on error.
int p8_cart_load(lua_State *L, const char *path);

// Load and run a .p8 cartridge from SD card.
// Parses the file, loads data into vram, preprocesses lua, runs _init,
// then enters the _update/_draw game loop. Returns when cart stops.
// Returns 0 on success, nonzero on error.
int p8_cart_run(lua_State *L, const char *path);

// Run the PICO-8 game loop on the current Lua state.
// Assumes _init/_update/_draw are already defined as globals.
// Calls _init() then enters _update/_draw loop.
// Returns when the cart calls stop() or encounters an error.
void p8_cart_gameloop(lua_State *L);

// Resume a stopped game loop without calling _init().
void p8_cart_gameloop_resume(lua_State *L);

// Register cart-related Lua globals (load, reset, run).
void p8_cart_register(lua_State *L);

// Get the path of the currently loaded cartridge (empty string if none).
const char *p8_cart_get_path(void);

// Reload data from the cart file. Re-reads the .p8 file and copies
// virtual memory range [src, src+len) from the fresh parse to [dest, dest+len)
// in the current virtual memory. If filename is NULL, uses current cart path.
// Returns 0 on success, -1 on error.
int p8_cart_reload(int dest, int src, int len, const char *filename);

// Write virtual memory range [src, src+len) from the current virtual memory
// to [dest, dest+len) in the cart's data sections on SD. Counterpart to reload().
// If filename is NULL, uses current cart path.
// Returns 0 on success, -1 on error.
int p8_cart_cstore(int dest, int src, int len, const char *filename);

#ifdef __cplusplus
}
#endif

#endif // P8_CART_H
