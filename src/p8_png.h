#ifndef P8_PNG_H
#define P8_PNG_H

#include <stdint.h>
#include <stddef.h>
#include "tlsf/tlsf.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the PNG loader with TLSF allocator
void p8_png_init(tlsf_t tlsf);

// Parse a .p8.png file buffer. Extracts cart data (sprites, map, etc.)
// into PICO-8 virtual memory and returns the decompressed Lua code
// as a newly allocated string (caller must tlsf_free).
// Sets *lua_len to the code length. Returns NULL on failure.
char *p8_png_load(const uint8_t *data, size_t data_len, size_t *lua_len);

#ifdef __cplusplus
}
#endif

#endif // P8_PNG_H
