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
    { HID_KEY_ARROW_LEFT,  0, 0 },                 // btn 0: left
    { HID_KEY_ARROW_RIGHT, 0, 0 },                 // btn 1: right
    { HID_KEY_ARROW_UP,    0, 0 },                 // btn 2: up
    { HID_KEY_ARROW_DOWN,  0, 0 },                 // btn 3: down
    { HID_KEY_Z, HID_KEY_C, HID_KEY_N },           // btn 4: O
    { HID_KEY_X, HID_KEY_V, HID_KEY_M },           // btn 5: X
    { HID_KEY_P, HID_KEY_ENTER, HID_KEY_ESCAPE },  // btn 6: menu
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
// Gamepad maps to P2 buttons via key_state bitfield (reuses P2 keycodes)
// We pick unused HID keycodes (0xE8-0xEF) as virtual gamepad keys
#define VKEY_GP_LEFT  0xE8
#define VKEY_GP_RIGHT 0xE9
#define VKEY_GP_UP    0xEA
#define VKEY_GP_DOWN  0xEB
#define VKEY_GP_O     0xEC  // btn 4 (face button 1 / A / South)
#define VKEY_GP_X     0xED  // btn 5 (face button 2 / B / East)

void input_gamepad_report(const uint8_t *report, uint16_t len) {
    if (len < 3) return;

    // Common gamepad HID report layout heuristic:
    // Byte 0: X axis (0-255, center ~128)
    // Byte 1: Y axis (0-255, center ~128)
    // Byte 2+: buttons bitmask
    // Some gamepads have a report ID as byte 0 — detect by checking if
    // byte 0 looks like a small report ID (< 16) and byte 1+2 are axis-like
    int axis_off = 0;
    if (len >= 4 && report[0] < 16 && report[1] >= 64 && report[1] <= 192) {
        axis_off = 1; // skip report ID
    }

    if ((int)len < axis_off + 3) return;

    uint8_t x_axis = report[axis_off];
    uint8_t y_axis = report[axis_off + 1];
    uint8_t buttons = report[axis_off + 2];

    // D-pad from axis values (dead zone: 64-192 = center)
    bool left  = (x_axis < 64);
    bool right = (x_axis > 192);
    bool up    = (y_axis < 64);
    bool down  = (y_axis > 192);

    // Also check hat switch if present in buttons byte
    // Some gamepads encode d-pad as hat: lower nibble of buttons byte
    // Hat: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8/15=center
    if (!left && !right && !up && !down && (buttons & 0x0F) <= 8) {
        uint8_t hat = buttons & 0x0F;
        if (hat == 0 || hat == 1 || hat == 7) up = true;
        if (hat == 4 || hat == 3 || hat == 5) down = true;
        if (hat == 6 || hat == 5 || hat == 7) left = true;
        if (hat == 2 || hat == 1 || hat == 3) right = true;
        // Use upper nibble or next byte for face buttons
        if (axis_off + 3 < (int)len) {
            buttons = report[axis_off + 3]; // face buttons in next byte
        } else {
            buttons >>= 4; // face buttons in upper nibble
        }
    }

    // Update virtual gamepad keys in key_state
    if (left)  key_set(key_state, VKEY_GP_LEFT);  else key_clear(key_state, VKEY_GP_LEFT);
    if (right) key_set(key_state, VKEY_GP_RIGHT); else key_clear(key_state, VKEY_GP_RIGHT);
    if (up)    key_set(key_state, VKEY_GP_UP);    else key_clear(key_state, VKEY_GP_UP);
    if (down)  key_set(key_state, VKEY_GP_DOWN);  else key_clear(key_state, VKEY_GP_DOWN);

    // Face buttons: bit 0 = A/South → O, bit 1 = B/East → X
    if (buttons & 0x01) key_set(key_state, VKEY_GP_O); else key_clear(key_state, VKEY_GP_O);
    if (buttons & 0x02) key_set(key_state, VKEY_GP_X); else key_clear(key_state, VKEY_GP_X);
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
