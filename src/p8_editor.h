#ifndef P8_EDITOR_H
#define P8_EDITOR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"
#include "tlsf/tlsf.h"

// Initialize the editor subsystem. Call once after TLSF is ready.
void p8_editor_init(tlsf_t tlsf);

// Enter the editor loop. Allocates buffer on first call.
// If editor buffer is empty and a cart is loaded, auto-loads its __lua__ section.
// ESC returns to caller (state is preserved for re-entry).
void p8_editor_enter(void);

// Load a file into the editor buffer (without entering editor mode).
bool p8_editor_load(const char *path);

// Save the editor buffer. If path is non-NULL/non-empty, sets the filename first.
// Adds .p8 extension if missing. Returns true on success.
bool p8_editor_save(const char *path);

// Register "edit" and "save" Lua globals.
void p8_editor_register(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // P8_EDITOR_H
