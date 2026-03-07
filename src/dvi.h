#ifndef DVI_H
#define DVI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize HSTX DVI output for 640x480@60Hz.
// framebuffer: pointer to 128x128 RGB565 buffer (32768 bytes)
// The display will continuously scan out this buffer via DMA,
// with 3x horizontal scaling and 3x vertical scaling (128->384 centered in 640x480).
void dvi_init(uint16_t *framebuffer);

#ifdef __cplusplus
}
#endif

#endif // DVI_H
