#include "input.h"
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#include "tusb.h"

// 256-bit bitfield for all HID keycodes (32 bytes)
static uint8_t key_state[32];
static uint8_t key_prev[32];

// Character input ring buffer for REPL keyboard input
#define CHAR_BUF_SIZE 64
static char char_buf[CHAR_BUF_SIZE];
static volatile int char_head = 0;
static volatile int char_tail = 0;

// btnp auto-repeat counters: [player][button]
// 0 = not held, 1..15 = initial delay, 16+ = repeating every 4 frames
static uint8_t btnp_hold[2][7]; // 7 buttons: 0-5 + menu(6)

// PICO-8 button keycodes: [player][button][alternatives]
// btn 0=left, 1=right, 2=up, 3=down, 4=O, 5=X, 6=menu
#define MAX_ALTS 3

static const uint8_t p1_keys[7][MAX_ALTS] = {
    { HID_KEY_ARROW_LEFT,  0xF0, 0 },              // btn 0: left (+ gamepad)
    { HID_KEY_ARROW_RIGHT, 0xF1, 0 },              // btn 1: right (+ gamepad)
    { HID_KEY_ARROW_UP,    0xF2, 0 },              // btn 2: up (+ gamepad)
    { HID_KEY_ARROW_DOWN,  0xF3, 0 },              // btn 3: down (+ gamepad)
    { HID_KEY_Z, HID_KEY_C, 0xF4 },                // btn 4: O (+ gamepad A)
    { HID_KEY_X, HID_KEY_V, 0xF5 },                // btn 5: X (+ gamepad B)
    { HID_KEY_P, HID_KEY_ENTER, 0xF6 },              // btn 6: menu (+ gamepad start)
};

static const uint8_t p2_keys[7][MAX_ALTS] = {
    { HID_KEY_S, 0xE8, 0 },                        // btn 0: left (+ gamepad)
    { HID_KEY_F, 0xE9, 0 },                        // btn 1: right (+ gamepad)
    { HID_KEY_E, 0xEA, 0 },                        // btn 2: up (+ gamepad)
    { HID_KEY_D, 0xEB, 0 },                        // btn 3: down (+ gamepad)
    { HID_KEY_TAB, HID_KEY_SHIFT_LEFT, 0xEC },     // btn 4: O (+ gamepad A)
    { HID_KEY_Q, HID_KEY_A, 0xED },                // btn 5: X (+ gamepad B)
    { 0, 0, 0 },                                   // btn 6: menu (P1 only)
};

static inline bool key_is_set(const uint8_t *bitfield, uint8_t keycode) {
    return (bitfield[keycode >> 3] & (1u << (keycode & 7))) != 0;
}

static inline void key_set(uint8_t *bitfield, uint8_t keycode) {
    bitfield[keycode >> 3] |= (1u << (keycode & 7));
}

static inline void key_clear(uint8_t *bitfield, uint8_t keycode) {
    bitfield[keycode >> 3] &= ~(1u << (keycode & 7));
}

// Current modifier state, updated each frame from the keyboard driver
static volatile uint8_t current_modifiers;
static input_modifier_poll_fn modifier_poll_fn;

void input_sync_modifiers(uint8_t modifiers) {
    current_modifiers = modifiers;
    // Mirror modifier bits into key_state so input_key(0xE0..0xE7) works
    for (int i = 0; i < 8; i++) {
        if (modifiers & (1 << i))
            key_set(key_state, 0xE0 + i);
        else
            key_clear(key_state, 0xE0 + i);
    }
}

void input_key_callback(uint8_t keycode, char ascii, bool pressed, uint8_t modifiers) {
    input_sync_modifiers(modifiers);

    if (pressed) {
        key_set(key_state, keycode);

        // Don't buffer chars when Ctrl is held (editor handles Ctrl combos via keycodes)
        bool ctrl = modifiers & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL);
        if (ctrl) return;

        // PICO-8: Shift+letter → P8SCII 128-153 (special wide glyphs)
        unsigned char buf_ch = (unsigned char)ascii;
        if (ascii >= 'A' && ascii <= 'Z' &&
            (modifiers & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))) {
            buf_ch = 128 + (ascii - 'A');
        }
        // Buffer printable ASCII + P8SCII, enter, and backspace
        if ((buf_ch >= 32 && buf_ch < 127) || buf_ch >= 128) {
            int next = (char_head + 1) % CHAR_BUF_SIZE;
            if (next != char_tail) {
                char_buf[char_head] = (char)buf_ch;
                char_head = next;
            }
        } else if (keycode == 0x28) { // HID_KEY_ENTER
            int next = (char_head + 1) % CHAR_BUF_SIZE;
            if (next != char_tail) {
                char_buf[char_head] = '\n';
                char_head = next;
            }
        } else if (keycode == 0x2A) { // HID_KEY_BACKSPACE
            int next = (char_head + 1) % CHAR_BUF_SIZE;
            if (next != char_tail) {
                char_buf[char_head] = '\b';
                char_head = next;
            }
        }
    } else {
        key_clear(key_state, keycode);
    }
}

int input_getchar(void) {
    if (char_tail == char_head) return -1;
    char c = char_buf[char_tail];
    char_tail = (char_tail + 1) % CHAR_BUF_SIZE;
    return (unsigned char)c;
}

void input_flush(void) {
    char_head = 0;
    char_tail = 0;
}

// --- Mouse state ---
static input_mouse_poll_fn mouse_poll_fn = NULL;

void input_set_mouse_poll(input_mouse_poll_fn fn) { mouse_poll_fn = fn; }

// Absolute cursor position on 128x128 screen, accumulated from USB deltas
static int mouse_x = 64;
static int mouse_y = 64;
static uint8_t mouse_btn = 0;
static int mouse_wheel_accum = 0;

void input_mouse_update(int32_t dx, int32_t dy, int32_t wheel, uint8_t buttons) {
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x > 127) mouse_x = 127;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y > 127) mouse_y = 127;
    mouse_wheel_accum += wheel;
    mouse_btn = buttons;
}

int input_mouse_x(void) { return mouse_x; }
int input_mouse_y(void) { return mouse_y; }
uint8_t input_mouse_buttons(void) { return mouse_btn; }
int input_mouse_wheel(void) {
    int w = mouse_wheel_accum;
    mouse_wheel_accum = 0;
    return w;
}
void input_mouse_reset(void) {
    mouse_x = 64;
    mouse_y = 64;
    mouse_btn = 0;
    mouse_wheel_accum = 0;
}

void input_set_modifier_poll(input_modifier_poll_fn fn) { modifier_poll_fn = fn; }

void input_update(void) {
    // Poll USB host so new HID reports are processed
    tuh_task();

    // Sync modifier keys from keyboard driver (Ctrl, Shift, Alt, GUI)
    if (modifier_poll_fn) input_sync_modifiers(modifier_poll_fn());

    // Poll mouse if registered
    if (mouse_poll_fn) mouse_poll_fn();

    // Update btnp hold counters for each player/button
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < 7; i++) {
            if (input_btn(i, p)) {
                if (btnp_hold[p][i] < 255)
                    btnp_hold[p][i]++;
            } else {
                btnp_hold[p][i] = 0;
            }
        }
    }

    // Copy current state to previous (for edge detection)
    memcpy(key_prev, key_state, sizeof(key_prev));
}

bool input_btn(int i, int player) {
    if (i < 0 || i > 6 || player < 0 || player > 1) return false;

    const uint8_t (*keys)[MAX_ALTS] = (player == 0) ? p1_keys : p2_keys;
    for (int a = 0; a < MAX_ALTS; a++) {
        uint8_t kc = keys[i][a];
        if (kc && key_is_set(key_state, kc))
            return true;
    }
    return false;
}

bool input_btnp(int i, int player) {
    if (i < 0 || i > 6 || player < 0 || player > 1) return false;

    if (!input_btn(i, player)) return false;

    uint8_t hold = btnp_hold[player][i];
    // Frame 1: just pressed (input_update already incremented from 0 to 1)
    if (hold == 1) return true;
    // Initial delay: frames 2-15 (14 frames)
    if (hold < 16) return false;
    // Auto-repeat every 4 frames after initial delay
    if ((hold - 16) % 4 == 0) return true;
    return false;
}

bool input_key(uint8_t keycode) {
    return key_is_set(key_state, keycode);
}

// --- Gamepad state ---
// Virtual gamepad keys per player, stored in unused HID keycode space
// Player 1 (0xF0-0xF5), Player 2 (0xE8-0xED)
#define VKEY_GP1_LEFT  0xF0
#define VKEY_GP1_RIGHT 0xF1
#define VKEY_GP1_UP    0xF2
#define VKEY_GP1_DOWN  0xF3
#define VKEY_GP1_O     0xF4  // btn 4 (face button 1 / A / South)
#define VKEY_GP1_X     0xF5  // btn 5 (face button 2 / B / East)
#define VKEY_GP1_MENU  0xF6  // btn 6 (start/menu, P1 only)

#define VKEY_GP2_LEFT  0xE8
#define VKEY_GP2_RIGHT 0xE9
#define VKEY_GP2_UP    0xEA
#define VKEY_GP2_DOWN  0xEB
#define VKEY_GP2_O     0xEC  // btn 4 (face button 1 / A / South)
#define VKEY_GP2_X     0xED  // btn 5 (face button 2 / B / East)

// Virtual keycodes indexed by player: [player][button]
// btn order: left, right, up, down, O, X, menu
static const uint8_t gamepad_vkeys[2][7] = {
    { VKEY_GP1_LEFT, VKEY_GP1_RIGHT, VKEY_GP1_UP, VKEY_GP1_DOWN, VKEY_GP1_O, VKEY_GP1_X, VKEY_GP1_MENU },
    { VKEY_GP2_LEFT, VKEY_GP2_RIGHT, VKEY_GP2_UP, VKEY_GP2_DOWN, VKEY_GP2_O, VKEY_GP2_X, 0 },
};

void input_gamepad_report(const uint8_t *report, uint16_t len, int player) {
    if (len < 3) return;
    if (player < 0 || player > 1) return;

    const uint8_t *vk = gamepad_vkeys[player];

    // Most controllers send: [buttons_lo, buttons_hi, x_axis, y_axis, ...]
    // Match the format assumed by fwUSBHostHIDController::processReport.
    // Bytes 0-1 are button bitmask, bytes 2+ are axes (uint8, center ~128).
    if (len < 4) return;

    uint8_t x_axis = report[2];
    uint8_t y_axis = report[3];
    uint16_t buttons_word = report[0] | ((uint16_t)report[1] << 8);

    // D-pad from axis values (dead zone: 64-192 = center)
    bool left  = (x_axis < 64);
    bool right = (x_axis > 192);
    bool up    = (y_axis < 64);
    bool down  = (y_axis > 192);

    // Also check hat switch if present in low nibble of buttons word
    // Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8/15=center
    uint8_t face_buttons = 0;
    if (!left && !right && !up && !down && (buttons_word & 0x0F) <= 8) {
        uint8_t hat = buttons_word & 0x0F;
        if (hat == 0 || hat == 1 || hat == 7) up = true;
        if (hat == 4 || hat == 3 || hat == 5) down = true;
        if (hat == 6 || hat == 5 || hat == 7) left = true;
        if (hat == 2 || hat == 1 || hat == 3) right = true;
        face_buttons = (uint8_t)(buttons_word >> 4);
    } else {
        face_buttons = (uint8_t)buttons_word;
    }

    // Update virtual gamepad keys in key_state
    if (left)  key_set(key_state, vk[0]);  else key_clear(key_state, vk[0]);
    if (right) key_set(key_state, vk[1]);  else key_clear(key_state, vk[1]);
    if (up)    key_set(key_state, vk[2]);  else key_clear(key_state, vk[2]);
    if (down)  key_set(key_state, vk[3]);  else key_clear(key_state, vk[3]);

    // Face buttons: bit 0 = A/South → O, bit 1 = B/East → X
    if (face_buttons & 0x01) key_set(key_state, vk[4]); else key_clear(key_state, vk[4]);
    if (face_buttons & 0x02) key_set(key_state, vk[5]); else key_clear(key_state, vk[5]);
}

// XInput defines (must match xinput_host.h)
#define XINPUT_GAMEPAD_DPAD_UP    0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN  0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT  0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START       0x0010
#define XINPUT_GAMEPAD_A          0x1000
#define XINPUT_GAMEPAD_B          0x2000

#define XINPUT_STICK_DEADZONE 8000

void input_xinput_update(uint16_t wButtons, int16_t stickLX, int16_t stickLY, int player) {
    if (player < 0 || player > 1) return;

    const uint8_t *vk = gamepad_vkeys[player];

    // D-pad buttons
    bool left  = (wButtons & XINPUT_GAMEPAD_DPAD_LEFT);
    bool right = (wButtons & XINPUT_GAMEPAD_DPAD_RIGHT);
    bool up    = (wButtons & XINPUT_GAMEPAD_DPAD_UP);
    bool down  = (wButtons & XINPUT_GAMEPAD_DPAD_DOWN);

    // Left stick as additional d-pad input (with dead zone)
    if (stickLX < -XINPUT_STICK_DEADZONE) left = true;
    if (stickLX >  XINPUT_STICK_DEADZONE) right = true;
    if (stickLY >  XINPUT_STICK_DEADZONE) up = true;
    if (stickLY < -XINPUT_STICK_DEADZONE) down = true;

    if (left)  key_set(key_state, vk[0]);  else key_clear(key_state, vk[0]);
    if (right) key_set(key_state, vk[1]);  else key_clear(key_state, vk[1]);
    if (up)    key_set(key_state, vk[2]);  else key_clear(key_state, vk[2]);
    if (down)  key_set(key_state, vk[3]);  else key_clear(key_state, vk[3]);

    // A → O (btn 4), B → X (btn 5)
    if (wButtons & XINPUT_GAMEPAD_A) key_set(key_state, vk[4]); else key_clear(key_state, vk[4]);
    if (wButtons & XINPUT_GAMEPAD_B) key_set(key_state, vk[5]); else key_clear(key_state, vk[5]);

    // Start → menu (btn 6, P1 only)
    if (vk[6]) {
        if (wButtons & XINPUT_GAMEPAD_START) key_set(key_state, vk[6]); else key_clear(key_state, vk[6]);
    }
}

// --- Lua bindings ---

static int l_btn(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    int p = (int)luaL_optinteger(L, 2, 0);
    lua_pushboolean(L, input_btn(i, p));
    return 1;
}

static int l_btnp(lua_State *L) {
    int i = (int)luaL_checkinteger(L, 1);
    int p = (int)luaL_optinteger(L, 2, 0);
    lua_pushboolean(L, input_btnp(i, p));
    return 1;
}

static int l_key(lua_State *L) {
    int keycode = (int)luaL_checkinteger(L, 1);
    if (keycode < 0 || keycode > 255) {
        lua_pushboolean(L, 0);
    } else {
        lua_pushboolean(L, input_key((uint8_t)keycode));
    }
    return 1;
}

static int l_update(lua_State *L) {
    (void)L;
    input_update();
    return 0;
}

static int l_debug(lua_State *L) {
    (void)L;
    // Check all possible device addresses for mounted HID devices
    printf("USB Host debug:\n");
    printf("  tuh_inited: %s\n", tuh_inited() ? "yes" : "no");
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
        if (tuh_mounted(addr)) {
            printf("  Device addr %d: mounted\n", addr);
            uint8_t itf_count = tuh_hid_itf_get_count(addr);
            printf("    HID interfaces: %d\n", itf_count);
            for (uint8_t itf = 0; itf < itf_count; itf++) {
                uint8_t proto = tuh_hid_interface_protocol(addr, itf);
                printf("    itf %d: proto=%d (%s)\n", itf, proto,
                    proto == 1 ? "keyboard" : proto == 2 ? "mouse" : "other");
            }
        }
    }
    // Show raw key_state — any bits set?
    int any_keys = 0;
    for (int i = 0; i < 32; i++) {
        if (key_state[i]) { any_keys = 1; break; }
    }
    printf("  Keys held: %s\n", any_keys ? "yes" : "none");
    if (any_keys) {
        for (int i = 0; i < 256; i++) {
            if (key_is_set(key_state, (uint8_t)i)) {
                printf("    keycode %d\n", i);
            }
        }
    }
    return 0;
}

static const luaL_Reg inputlib[] = {
    {"btn",    l_btn},
    {"btnp",   l_btnp},
    {"key",    l_key},
    {"update", l_update},
    {"debug",  l_debug},
    {NULL, NULL}
};

int luaopen_input(lua_State *L) {
    luaL_newlib(L, inputlib);
    return 1;
}
