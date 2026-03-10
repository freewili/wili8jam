#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lua.h"

// Key event callback (matches KeyEventCallback signature in fwUSBHostHIDKeyboard.h)
void input_key_callback(uint8_t keycode, char ascii, bool pressed, uint8_t modifiers);

// Call once per frame to update btnp edge detection and auto-repeat
void input_update(void);

// PICO-8 btn(i, player): true if button i is currently held
// i: 0=left, 1=right, 2=up, 3=down, 4=O(z/c/n), 5=X(x/v/m)
// player: 0 or 1
bool input_btn(int i, int player);

// PICO-8 btnp(i, player): true on initial press, then auto-repeat (15 frame delay, 4 frame repeat)
bool input_btnp(int i, int player);

// Raw key state: true if HID keycode is currently held
bool input_key(uint8_t keycode);

// Modifier key polling (call input_set_modifier_poll once at init)
typedef uint8_t (*input_modifier_poll_fn)(void);
void input_set_modifier_poll(input_modifier_poll_fn fn);

// Get next character from USB keyboard buffer. Returns -1 if empty.
int input_getchar(void);

// Flush the keyboard character buffer (discard all buffered input).
void input_flush(void);

// Mouse state (updated externally from C++ USB host driver)
typedef void (*input_mouse_poll_fn)(void);
void input_set_mouse_poll(input_mouse_poll_fn fn);
void input_mouse_update(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons);

// Gamepad state (updated from USB HID controller/generic reports)
// player: 0 or 1 (maps to PICO-8 player index)
void input_gamepad_report(const uint8_t *report, uint16_t len, int player);

// DualSense/DualShock report (Sony controllers via HID)
// Raw report from controller callback; pid used to select byte offsets
void input_dualsense_report(const uint8_t *report, uint16_t len, int player, uint16_t pid);

// XInput gamepad state (d-pad bitmask + left stick, already normalized)
// wButtons uses XINPUT_GAMEPAD_* defines, stickLX/LY are signed 16-bit
void input_xinput_update(uint16_t wButtons, int16_t stickLX, int16_t stickLY, int player);
int input_mouse_x(void);
int input_mouse_y(void);
uint8_t input_mouse_buttons(void);
int input_mouse_wheel(void);
void input_mouse_reset(void);

// Lua library opener
int luaopen_input(lua_State *L);

#ifdef __cplusplus
}
#endif

#endif // INPUT_H
