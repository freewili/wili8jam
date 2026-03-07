/*
 * Graphics library for wili8jam: 128x128 4-bit indexed framebuffer
 * with PICO-8 palette, drawing primitives, bitmap font, and Lua bindings.
 */

#include "gfx.h"
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "p8_sfx.h"

// --- Framebuffers ---

// 4-bit indexed framebuffer: 2 pixels per byte, 128*128/2 = 8192 bytes
// PICO-8 nibble order: low nibble = left pixel (even x), high nibble = right pixel (odd x)
#define FB4_SIZE (GFX_WIDTH * GFX_HEIGHT / 2)
static uint8_t fb4_default[FB4_SIZE];
static uint8_t *fb4 = fb4_default;

// RGB565 DVI framebuffer: 128*128*2 = 32768 bytes
static uint16_t fb_dvi[GFX_WIDTH * GFX_HEIGHT];

// Display palette: if set, maps framebuffer color → display color before palette lookup
static const uint8_t *gfx_disp_pal = NULL;

// Current default draw color
static int draw_color = 7; // white

// --- PICO-8 palette (RGB565, big-endian for DVI with BSWAP) ---
// Standard RGB888 -> RGB565: R>>3 << 11 | G>>2 << 5 | B>>3

static const uint16_t palette[16] = {
    0x0000, // 0: #000000 black
    0x194A, // 1: #1D2B53 dark blue
    0x792A, // 2: #7E2553 dark purple
    0x042A, // 3: #008751 dark green
    0xAA86, // 4: #AB5236 brown
    0x5AA9, // 5: #5F574F dark grey
    0xC618, // 6: #C2C3C7 light grey
    0xFFBD, // 7: #FFF1E8 white
    0xF809, // 8: #FF004D red
    0xFD00, // 9: #FFA300 orange
    0xFF64, // 10: #FFEC27 yellow
    0x0726, // 11: #00E436 green
    0x2D7F, // 12: #29ADFF blue
    0x83B3, // 13: #83769C indigo
    0xFBB5, // 14: #FF77A8 pink
    0xFE75, // 15: #FFCCAA peach
};

// --- 4x6 bitmap font (printable ASCII 32..126) ---
// Each character is 4 pixels wide, 6 pixels tall.
// Stored as 6 bytes per char, each byte has 4 low bits = pixels (MSB=left).

// PICO-8 font (from yocto-8, MIT license). LSB = leftmost pixel, 3px wide.
static const uint8_t font_4x6[][6] = {
    {0x00,0x00,0x00,0x00,0x00,0x00}, // 32: space
    {0x02,0x02,0x02,0x00,0x02,0x00}, // 33: !
    {0x05,0x05,0x00,0x00,0x00,0x00}, // 34: "
    {0x05,0x07,0x05,0x07,0x05,0x00}, // 35: #
    {0x07,0x03,0x06,0x07,0x02,0x00}, // 36: $
    {0x05,0x04,0x02,0x01,0x05,0x00}, // 37: %
    {0x03,0x03,0x06,0x05,0x07,0x00}, // 38: &
    {0x02,0x01,0x00,0x00,0x00,0x00}, // 39: '
    {0x02,0x01,0x01,0x01,0x02,0x00}, // 40: (
    {0x02,0x04,0x04,0x04,0x02,0x00}, // 41: )
    {0x05,0x02,0x07,0x02,0x05,0x00}, // 42: *
    {0x00,0x02,0x07,0x02,0x00,0x00}, // 43: +
    {0x00,0x00,0x00,0x02,0x01,0x00}, // 44: ,
    {0x00,0x00,0x07,0x00,0x00,0x00}, // 45: -
    {0x00,0x00,0x00,0x00,0x02,0x00}, // 46: .
    {0x04,0x02,0x02,0x02,0x01,0x00}, // 47: /
    {0x07,0x05,0x05,0x05,0x07,0x00}, // 48: 0
    {0x03,0x02,0x02,0x02,0x07,0x00}, // 49: 1
    {0x07,0x04,0x07,0x01,0x07,0x00}, // 50: 2
    {0x07,0x04,0x06,0x04,0x07,0x00}, // 51: 3
    {0x05,0x05,0x07,0x04,0x04,0x00}, // 52: 4
    {0x07,0x01,0x07,0x04,0x07,0x00}, // 53: 5
    {0x01,0x01,0x07,0x05,0x07,0x00}, // 54: 6
    {0x07,0x04,0x04,0x04,0x04,0x00}, // 55: 7
    {0x07,0x05,0x07,0x05,0x07,0x00}, // 56: 8
    {0x07,0x05,0x07,0x04,0x04,0x00}, // 57: 9
    {0x00,0x02,0x00,0x02,0x00,0x00}, // 58: :
    {0x00,0x02,0x00,0x02,0x01,0x00}, // 59: ;
    {0x04,0x02,0x01,0x02,0x04,0x00}, // 60: <
    {0x00,0x07,0x00,0x07,0x00,0x00}, // 61: =
    {0x01,0x02,0x04,0x02,0x01,0x00}, // 62: >
    {0x07,0x04,0x06,0x00,0x02,0x00}, // 63: ?
    {0x02,0x05,0x05,0x01,0x06,0x00}, // 64: @
    {0x00,0x06,0x05,0x07,0x05,0x00}, // 65: A
    {0x00,0x03,0x03,0x05,0x07,0x00}, // 66: B
    {0x00,0x06,0x01,0x01,0x06,0x00}, // 67: C
    {0x00,0x03,0x05,0x05,0x03,0x00}, // 68: D
    {0x00,0x07,0x03,0x01,0x06,0x00}, // 69: E
    {0x00,0x07,0x03,0x01,0x01,0x00}, // 70: F
    {0x00,0x06,0x01,0x05,0x07,0x00}, // 71: G
    {0x00,0x05,0x05,0x07,0x05,0x00}, // 72: H
    {0x00,0x07,0x02,0x02,0x07,0x00}, // 73: I
    {0x00,0x07,0x02,0x02,0x03,0x00}, // 74: J
    {0x00,0x05,0x03,0x05,0x05,0x00}, // 75: K
    {0x00,0x01,0x01,0x01,0x06,0x00}, // 76: L
    {0x00,0x07,0x07,0x05,0x05,0x00}, // 77: M
    {0x00,0x03,0x05,0x05,0x05,0x00}, // 78: N
    {0x00,0x06,0x05,0x05,0x03,0x00}, // 79: O
    {0x00,0x06,0x05,0x07,0x01,0x00}, // 80: P
    {0x00,0x02,0x05,0x03,0x06,0x00}, // 81: Q
    {0x00,0x03,0x05,0x03,0x05,0x00}, // 82: R
    {0x00,0x06,0x01,0x04,0x03,0x00}, // 83: S
    {0x00,0x07,0x02,0x02,0x02,0x00}, // 84: T
    {0x00,0x05,0x05,0x05,0x06,0x00}, // 85: U
    {0x00,0x05,0x05,0x07,0x02,0x00}, // 86: V
    {0x00,0x05,0x05,0x07,0x07,0x00}, // 87: W
    {0x00,0x05,0x02,0x02,0x05,0x00}, // 88: X
    {0x00,0x05,0x07,0x04,0x03,0x00}, // 89: Y
    {0x00,0x07,0x04,0x01,0x07,0x00}, // 90: Z
    {0x03,0x01,0x01,0x01,0x03,0x00}, // 91: [
    {0x01,0x02,0x02,0x02,0x04,0x00}, // 92: backslash
    {0x06,0x04,0x04,0x04,0x06,0x00}, // 93: ]
    {0x02,0x05,0x00,0x00,0x00,0x00}, // 94: ^
    {0x00,0x00,0x00,0x00,0x07,0x00}, // 95: _
    {0x02,0x04,0x00,0x00,0x00,0x00}, // 96: `
    {0x07,0x05,0x07,0x05,0x05,0x00}, // 97: a
    {0x07,0x05,0x03,0x05,0x07,0x00}, // 98: b
    {0x06,0x01,0x01,0x01,0x06,0x00}, // 99: c
    {0x03,0x05,0x05,0x05,0x07,0x00}, // 100: d
    {0x07,0x01,0x03,0x01,0x07,0x00}, // 101: e
    {0x07,0x01,0x03,0x01,0x01,0x00}, // 102: f
    {0x06,0x01,0x01,0x05,0x07,0x00}, // 103: g
    {0x05,0x05,0x07,0x05,0x05,0x00}, // 104: h
    {0x07,0x02,0x02,0x02,0x07,0x00}, // 105: i
    {0x07,0x02,0x02,0x02,0x03,0x00}, // 106: j
    {0x05,0x05,0x03,0x05,0x05,0x00}, // 107: k
    {0x01,0x01,0x01,0x01,0x07,0x00}, // 108: l
    {0x07,0x07,0x05,0x05,0x05,0x00}, // 109: m
    {0x03,0x05,0x05,0x05,0x05,0x00}, // 110: n
    {0x06,0x05,0x05,0x05,0x03,0x00}, // 111: o
    {0x07,0x05,0x07,0x01,0x01,0x00}, // 112: p
    {0x02,0x05,0x05,0x03,0x06,0x00}, // 113: q
    {0x07,0x05,0x03,0x05,0x05,0x00}, // 114: r
    {0x06,0x01,0x07,0x04,0x03,0x00}, // 115: s
    {0x07,0x02,0x02,0x02,0x02,0x00}, // 116: t
    {0x05,0x05,0x05,0x05,0x06,0x00}, // 117: u
    {0x05,0x05,0x05,0x07,0x02,0x00}, // 118: v
    {0x05,0x05,0x05,0x07,0x07,0x00}, // 119: w
    {0x05,0x05,0x02,0x05,0x05,0x00}, // 120: x
    {0x05,0x05,0x07,0x04,0x07,0x00}, // 121: y
    {0x07,0x04,0x02,0x01,0x07,0x00}, // 122: z
    {0x06,0x02,0x03,0x02,0x06,0x00}, // 123: {
    {0x02,0x02,0x02,0x02,0x02,0x00}, // 124: |
    {0x03,0x02,0x06,0x02,0x03,0x00}, // 125: }
    {0x00,0x04,0x07,0x01,0x00,0x00}, // 126: ~
};

// --- P8SCII special characters (16-31), from yocto-8 ---
static const uint8_t font_p8_special[][6] = {
    {0x07,0x07,0x07,0x07,0x07,0x00}, // 16
    {0x00,0x07,0x07,0x07,0x00,0x00}, // 17
    {0x00,0x07,0x05,0x07,0x00,0x00}, // 18
    {0x00,0x05,0x02,0x05,0x00,0x00}, // 19
    {0x00,0x05,0x00,0x05,0x00,0x00}, // 20
    {0x00,0x05,0x05,0x05,0x00,0x00}, // 21
    {0x04,0x06,0x07,0x06,0x04,0x00}, // 22
    {0x01,0x03,0x07,0x03,0x01,0x00}, // 23
    {0x07,0x01,0x01,0x01,0x00,0x00}, // 24
    {0x00,0x04,0x04,0x04,0x07,0x00}, // 25
    {0x05,0x07,0x02,0x07,0x02,0x00}, // 26
    {0x00,0x00,0x02,0x00,0x00,0x00}, // 27
    {0x00,0x00,0x00,0x01,0x02,0x00}, // 28
    {0x00,0x00,0x00,0x03,0x03,0x00}, // 29
    {0x05,0x05,0x00,0x00,0x00,0x00}, // 30
    {0x02,0x05,0x02,0x00,0x00,0x00}, // 31
};

// --- P8SCII extended characters (128-255), 8px wide ---
// 6 bytes per glyph, 8 bits per row (bit7=leftmost)
// Sourced from yocto-8 (MIT), bit-reversed for MSB-left rendering
static const uint8_t font_p8_wide[][6] = {
    // 128-153: PICO-8 symbol glyphs
    {0xFE,0xFE,0xFE,0xFE,0xFE,0x00}, // 128: █ full block
    {0xAA,0x54,0xAA,0x54,0xAA,0x00}, // 129: ▒ medium shade
    {0x82,0xFE,0xBA,0xBA,0x7C,0x00}, // 130: 🐱 cat
    {0x7C,0xC6,0xC6,0xEE,0x7C,0x00}, // 131: ⬇️ down
    {0x88,0x22,0x88,0x22,0x88,0x00}, // 132: ░ light shade
    {0x20,0x3C,0x38,0x78,0x08,0x00}, // 133: ✽ sparkle
    {0x38,0x74,0x7C,0x7C,0x38,0x00}, // 134: ● circle
    {0x6C,0x7C,0x7C,0x38,0x10,0x00}, // 135: ♥ heart
    {0x38,0x6C,0xEE,0x6C,0x38,0x00}, // 136: ☉ sun
    {0x38,0x38,0x7C,0x38,0x28,0x00}, // 137: 웃 smiley
    {0x38,0x7C,0xFE,0x54,0x5C,0x00}, // 138: ⌂ house
    {0x7C,0xE6,0xC6,0xE6,0x7C,0x00}, // 139: ⬅️ left
    {0xFE,0xBA,0xFE,0x82,0xFE,0x00}, // 140: vert line
    {0x1C,0x10,0x10,0x70,0x70,0x00}, // 141: ♪ music
    {0x7C,0xC6,0xD6,0xC6,0x7C,0x00}, // 142: 🅾️ O button
    {0x10,0x38,0x7C,0x38,0x10,0x00}, // 143: ◆ diamond
    {0x00,0x00,0xAA,0x00,0x00,0x00}, // 144: … ellipsis
    {0x7C,0xCE,0xC6,0xCE,0x7C,0x00}, // 145: ➡️ right
    {0x10,0x38,0xFE,0x7C,0x44,0x00}, // 146: ★ star
    {0x7C,0x38,0x10,0x38,0x7C,0x00}, // 147: ⧗ hourglass
    {0x7C,0xEE,0xC6,0xC6,0x7C,0x00}, // 148: ⬆️ up
    {0x00,0xA0,0x4A,0x04,0x00,0x00}, // 149: ˇ caron
    {0x00,0x88,0x54,0x22,0x00,0x00}, // 150: ∧ wedge
    {0x7C,0xD6,0xEE,0xD6,0x7C,0x00}, // 151: ❎ X button
    {0xFE,0x00,0xFE,0x00,0xFE,0x00}, // 152: ▤ horiz lines
    {0xAA,0xAA,0xAA,0xAA,0xAA,0x00}, // 153: ▥ vert lines
    // 154-203: hiragana
    {0x70,0x20,0x78,0xB4,0x64,0x00}, // 154: あ
    {0x88,0x84,0x84,0xA4,0x40,0x00}, // 155: い
    {0x30,0x78,0x04,0x04,0x38,0x00}, // 156: う
    {0x10,0x78,0x10,0x24,0x58,0x00}, // 157: え
    {0x72,0x20,0x7C,0xA2,0x64,0x00}, // 158: お
    {0x44,0xFA,0x48,0x48,0x50,0x00}, // 159: か
    {0x78,0x10,0x3C,0x88,0x60,0x00}, // 160: き
    {0x08,0x30,0x40,0x30,0x08,0x00}, // 161: く
    {0x44,0x5E,0x44,0x44,0x48,0x00}, // 162: け
    {0x78,0x04,0x00,0x40,0x3C,0x00}, // 163: こ
    {0x10,0x3C,0x08,0x40,0x30,0x00}, // 164: さ
    {0x40,0x40,0x40,0x44,0x38,0x00}, // 165: し
    {0x10,0x7C,0x10,0x30,0x10,0x00}, // 166: す
    {0x48,0xFC,0x48,0x40,0x38,0x00}, // 167: せ
    {0x3C,0x08,0x7E,0x20,0x1C,0x00}, // 168: そ
    {0x40,0xE0,0x4C,0x40,0x4C,0x00}, // 169: た
    {0xF0,0x40,0x70,0x08,0x38,0x00}, // 170: ち
    {0x7C,0x02,0x02,0x04,0x18,0x00}, // 171: つ
    {0x7C,0x08,0x10,0x10,0x08,0x00}, // 172: て
    {0x10,0x1C,0x20,0x40,0x3C,0x00}, // 173: と
    {0x4C,0xE0,0x48,0x1E,0x18,0x00}, // 174: な
    {0x5E,0x42,0x40,0x50,0x4E,0x00}, // 175: に
    {0x90,0x7C,0xD2,0xB6,0x66,0x00}, // 176: ぬ
    {0x58,0xE4,0x44,0xCE,0x4C,0x00}, // 177: ね
    {0x3C,0x52,0x92,0x92,0x62,0x00}, // 178: の
    {0x48,0x5C,0x48,0x5C,0x58,0x00}, // 179: は
    {0xC4,0x46,0x44,0x44,0x38,0x00}, // 180: ひ
    {0x30,0x00,0x10,0x54,0xB2,0x00}, // 181: ふ
    {0x00,0x30,0x48,0x84,0x02,0x00}, // 182: へ
    {0xBE,0x9E,0x88,0xBC,0xBA,0x00}, // 183: ほ
    {0x7C,0x3C,0x10,0x78,0x74,0x00}, // 184: ま
    {0x60,0x24,0x7E,0x64,0x08,0x00}, // 185: み
    {0x24,0x72,0x20,0x62,0x3C,0x00}, // 186: む
    {0x50,0x3C,0x5A,0x62,0x0C,0x00}, // 187: め
    {0x78,0x20,0x78,0x22,0x1C,0x00}, // 188: も
    {0x28,0x7C,0x24,0x10,0x10,0x00}, // 189: や
    {0x5C,0x6A,0x4A,0x0C,0x10,0x00}, // 190: ゆ
    {0x20,0x38,0x20,0x78,0x60,0x00}, // 191: よ
    {0x10,0x40,0x7C,0x04,0x38,0x00}, // 192: ら
    {0x44,0x44,0x64,0x04,0x18,0x00}, // 193: り
    {0x7C,0x18,0x24,0x4E,0x0C,0x00}, // 194: る
    {0x20,0x6C,0x34,0x64,0x26,0x00}, // 195: れ
    {0x7C,0x18,0x24,0x42,0x0C,0x00}, // 196: ろ
    {0x58,0xE4,0x44,0xC4,0x48,0x00}, // 197: わ
    {0x70,0x26,0x38,0x14,0x1E,0x00}, // 198: を
    {0x20,0x40,0x60,0xD4,0x98,0x00}, // 199: ん
    {0x00,0x00,0x70,0x08,0x10,0x00}, // 200: っ
    {0x00,0x50,0xF8,0x48,0x20,0x00}, // 201: ゃ
    {0x00,0x20,0xF0,0xA8,0xB0,0x00}, // 202: ゅ
    {0x00,0x20,0x30,0x60,0x70,0x00}, // 203: ょ
    // 204-253: katakana
    {0x7C,0x04,0x28,0x20,0x40,0x00}, // 204: ア
    {0x0C,0x10,0x70,0x10,0x10,0x00}, // 205: イ
    {0x10,0x7C,0x44,0x04,0x18,0x00}, // 206: ウ
    {0x7C,0x10,0x10,0x10,0x7C,0x00}, // 207: エ
    {0x08,0x7E,0x18,0x28,0x48,0x00}, // 208: オ
    {0x20,0x7C,0x24,0x44,0x4C,0x00}, // 209: カ
    {0x10,0x7C,0x10,0x7C,0x10,0x00}, // 210: キ
    {0x3C,0x24,0x44,0x08,0x10,0x00}, // 211: ク
    {0x20,0x3E,0x48,0x08,0x10,0x00}, // 212: ケ
    {0x7C,0x04,0x04,0x04,0x7C,0x00}, // 213: コ
    {0x24,0x7E,0x24,0x04,0x08,0x00}, // 214: サ
    {0x60,0x04,0x64,0x08,0x30,0x00}, // 215: シ
    {0x7C,0x04,0x08,0x18,0x64,0x00}, // 216: ス
    {0x20,0x7C,0x24,0x20,0x1C,0x00}, // 217: セ
    {0x44,0x24,0x04,0x08,0x30,0x00}, // 218: ソ
    {0x7C,0x44,0xB4,0x0C,0x30,0x00}, // 219: タ
    {0x38,0x10,0x7C,0x10,0x20,0x00}, // 220: チ
    {0x54,0x54,0x04,0x08,0x30,0x00}, // 221: ツ
    {0x38,0x00,0x7C,0x10,0x20,0x00}, // 222: テ
    {0x20,0x20,0x38,0x24,0x20,0x00}, // 223: ト
    {0x10,0x7C,0x10,0x10,0x20,0x00}, // 224: ナ
    {0x00,0x38,0x00,0x00,0x7C,0x00}, // 225: ニ
    {0x7C,0x04,0x14,0x08,0x34,0x00}, // 226: ヌ
    {0x10,0x7C,0x0C,0x7A,0x10,0x00}, // 227: ネ
    {0x04,0x04,0x04,0x08,0x70,0x00}, // 228: ノ
    {0x08,0x24,0x24,0x22,0x42,0x00}, // 229: ハ
    {0x40,0x78,0x40,0x40,0x38,0x00}, // 230: ヒ
    {0x7C,0x04,0x04,0x08,0x30,0x00}, // 231: フ
    {0x30,0x48,0x84,0x02,0x00,0x00}, // 232: ヘ
    {0x10,0x7C,0x10,0x54,0x54,0x00}, // 233: ホ
    {0x7C,0x04,0x28,0x10,0x08,0x00}, // 234: マ
    {0x3C,0x00,0x7C,0x00,0x78,0x00}, // 235: ミ
    {0x10,0x20,0x24,0x42,0x7E,0x00}, // 236: ム
    {0x02,0x14,0x08,0x16,0x60,0x00}, // 237: メ
    {0x78,0x20,0x78,0x20,0x3C,0x00}, // 238: モ
    {0x20,0x7C,0x24,0x20,0x20,0x00}, // 239: ヤ
    {0x38,0x08,0x08,0x08,0x7C,0x00}, // 240: ユ
    {0x78,0x08,0x78,0x08,0x78,0x00}, // 241: ヨ
    {0x7C,0x00,0x7C,0x04,0x18,0x00}, // 242: ラ
    {0x24,0x24,0x24,0x04,0x08,0x00}, // 243: リ
    {0x28,0x28,0x28,0x2A,0x4C,0x00}, // 244: ル
    {0x40,0x40,0x44,0x48,0x70,0x00}, // 245: レ
    {0x7C,0x44,0x44,0x44,0x7C,0x00}, // 246: ロ
    {0x7C,0x44,0x04,0x08,0x30,0x00}, // 247: ワ
    {0x7C,0x04,0x3C,0x04,0x18,0x00}, // 248: ヲ
    {0x60,0x04,0x04,0x08,0x70,0x00}, // 249: ン
    {0x00,0xA8,0x08,0x10,0x60,0x00}, // 250: ッ
    {0x00,0x20,0x78,0x28,0x20,0x00}, // 251: ャ
    {0x00,0x00,0x30,0x10,0x78,0x00}, // 252: ュ
    {0x00,0x38,0x18,0x08,0x38,0x00}, // 253: ョ
    // 254-255: arc symbols
    {0x10,0x20,0xC6,0x08,0x10,0x00}, // 254: ◜
    {0x10,0x08,0xC6,0x20,0x10,0x00}, // 255: ◝
};

// --- Unicode → P8SCII conversion ---

// Map a Unicode code point to a P8SCII byte value. Returns -1 if unknown.
static int unicode_to_p8scii(uint32_t cp) {
    // Symbol glyphs (128-153)
    switch (cp) {
        case 0x2588: return 128;  // █
        case 0x2592: return 129;  // ▒
        case 0x1F431: return 130; // 🐱
        case 0x2B07: return 131;  // ⬇️
        case 0x2591: return 132;  // ░
        case 0x273D: return 133;  // ✽
        case 0x25CF: return 134;  // ●
        case 0x2665: return 135;  // ♥
        case 0x2609: return 136;  // ☉
        case 0xC6C3: return 137;  // 웃
        case 0x2302: return 138;  // ⌂
        case 0x2B05: return 139;  // ⬅️
        case 0x266A: return 141;  // ♪
        case 0x1F17E: return 142; // 🅾️
        case 0x25C6: return 143;  // ◆
        case 0x2026: return 144;  // …
        case 0x27A1: return 145;  // ➡️
        case 0x2605: return 146;  // ★
        case 0x29D7: return 147;  // ⧗
        case 0x2B06: return 148;  // ⬆️
        case 0x02C7: return 149;  // ˇ
        case 0x2227: return 150;  // ∧
        case 0x274E: return 151;  // ❎
        case 0x25A4: return 152;  // ▤
        case 0x25A5: return 153;  // ▥
        // Special display chars (16-31)
        case 0x25AE: return 16;   // ▮
        case 0x25A0: return 17;   // ■
        case 0x25A1: return 18;   // □
        case 0x2059: return 19;   // ⁙
        case 0x2058: return 20;   // ⁘
        case 0x2016: return 21;   // ‖
        case 0x25C0: return 22;   // ◀
        case 0x25B6: return 23;   // ▶
        case 0x300C: return 24;   // 「
        case 0x300D: return 25;   // 」
        case 0x00A5: return 26;   // ¥
        case 0x2022: return 27;   // •
        case 0x3001: return 28;   // 、
        case 0x3002: return 29;   // 。
        case 0x309B: return 30;   // ゛
        case 0x309C: return 31;   // ゜
        // Other
        case 0x25CB: return 127;  // ○
        case 0x25DC: return 254;  // ◜
        case 0x25DD: return 255;  // ◝
    }
    // Katakana gojuon → P8SCII 204-253
    {
        static const uint16_t kata[] = {
            0x30A2,0x30A4,0x30A6,0x30A8,0x30AA, // ア-オ
            0x30AB,0x30AD,0x30AF,0x30B1,0x30B3, // カ-コ
            0x30B5,0x30B7,0x30B9,0x30BB,0x30BD, // サ-ソ
            0x30BF,0x30C1,0x30C4,0x30C6,0x30C8, // タ-ト
            0x30CA,0x30CB,0x30CC,0x30CD,0x30CE, // ナ-ノ
            0x30CF,0x30D2,0x30D5,0x30D8,0x30DB, // ハ-ホ
            0x30DE,0x30DF,0x30E0,0x30E1,0x30E2, // マ-モ
            0x30E4,0x30E6,0x30E8,                 // ヤ-ヨ
            0x30E9,0x30EA,0x30EB,0x30EC,0x30ED, // ラ-ロ
            0x30EF,0x30F2,0x30F3,                 // ワ-ン
            0x30C3,0x30E3,0x30E5,0x30E7           // ッャュョ
        };
        for (int i = 0; i < 50; i++)
            if (cp == kata[i]) return 204 + i;
    }
    // Hiragana gojuon → P8SCII 154-203
    {
        static const uint16_t hira[] = {
            0x3042,0x3044,0x3046,0x3048,0x304A, // あ-お
            0x304B,0x304D,0x304F,0x3051,0x3053, // か-こ
            0x3055,0x3057,0x3059,0x305B,0x305D, // さ-そ
            0x305F,0x3061,0x3064,0x3066,0x3068, // た-と
            0x306A,0x306B,0x306C,0x306D,0x306E, // な-の
            0x306F,0x3072,0x3075,0x3078,0x307B, // は-ほ
            0x307E,0x307F,0x3080,0x3081,0x3082, // ま-も
            0x3084,0x3086,0x3088,                 // や-よ
            0x3089,0x308A,0x308B,0x308C,0x308D, // ら-ろ
            0x308F,0x3092,0x3093,                 // わ-ん
            0x3063,0x3083,0x3085,0x3087           // っゃゅょ
        };
        for (int i = 0; i < 50; i++)
            if (cp == hira[i]) return 154 + i;
    }
    return -1;
}

// --- Internal helpers ---

static inline void pset_fast(int x, int y, uint8_t c) {
    int idx = y * (GFX_WIDTH / 2) + (x >> 1);
    if (x & 1)
        fb4[idx] = (fb4[idx] & 0x0F) | (c << 4);
    else
        fb4[idx] = (fb4[idx] & 0xF0) | c;
}

static inline uint8_t pget_fast(int x, int y) {
    int idx = y * (GFX_WIDTH / 2) + (x >> 1);
    if (x & 1)
        return (fb4[idx] >> 4) & 0x0F;
    else
        return fb4[idx] & 0x0F;
}

static inline int clip(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int in_bounds(int x, int y) {
    return (unsigned)x < GFX_WIDTH && (unsigned)y < GFX_HEIGHT;
}

// --- Public drawing API ---

void gfx_init(void) {
    draw_color = 7;
    memset(fb4, 0, FB4_SIZE);
    memset(fb_dvi, 0, sizeof(fb_dvi));
}

uint16_t *gfx_get_dvi_buffer(void) {
    return fb_dvi;
}

void gfx_cls(int color) {
    uint8_t c = color & 0xF;
    uint8_t pair = (c << 4) | c;
    memset(fb4, pair, FB4_SIZE);
}

void gfx_pset(int x, int y, int c) {
    if (in_bounds(x, y))
        pset_fast(x, y, c & 0xF);
}

int gfx_pget(int x, int y) {
    if (in_bounds(x, y))
        return pget_fast(x, y);
    return 0;
}

void gfx_rect(int x0, int y0, int x1, int y1, int c) {
    uint8_t col = c & 0xF;
    // Normalize
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    x0 = clip(x0, 0, GFX_WIDTH - 1);
    x1 = clip(x1, 0, GFX_WIDTH - 1);
    y0 = clip(y0, 0, GFX_HEIGHT - 1);
    y1 = clip(y1, 0, GFX_HEIGHT - 1);

    for (int x = x0; x <= x1; x++) { pset_fast(x, y0, col); pset_fast(x, y1, col); }
    for (int y = y0 + 1; y < y1; y++) { pset_fast(x0, y, col); pset_fast(x1, y, col); }
}

void gfx_rectfill(int x0, int y0, int x1, int y1, int c) {
    uint8_t col = c & 0xF;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    x0 = clip(x0, 0, GFX_WIDTH - 1);
    x1 = clip(x1, 0, GFX_WIDTH - 1);
    y0 = clip(y0, 0, GFX_HEIGHT - 1);
    y1 = clip(y1, 0, GFX_HEIGHT - 1);

    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            pset_fast(x, y, col);
}

void gfx_line(int x0, int y0, int x1, int y1, int c) {
    uint8_t col = c & 0xF;
    int dx = x1 - x0;
    int dy = y1 - y0;
    int sx = dx > 0 ? 1 : -1;
    int sy = dy > 0 ? 1 : -1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;

    int err = dx - dy;

    for (;;) {
        if (in_bounds(x0, y0)) pset_fast(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

void gfx_circ(int cx, int cy, int r, int c) {
    uint8_t col = c & 0xF;
    if (r < 0) return;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        if (in_bounds(cx+x, cy+y)) pset_fast(cx+x, cy+y, col);
        if (in_bounds(cx-x, cy+y)) pset_fast(cx-x, cy+y, col);
        if (in_bounds(cx+x, cy-y)) pset_fast(cx+x, cy-y, col);
        if (in_bounds(cx-x, cy-y)) pset_fast(cx-x, cy-y, col);
        if (in_bounds(cx+y, cy+x)) pset_fast(cx+y, cy+x, col);
        if (in_bounds(cx-y, cy+x)) pset_fast(cx-y, cy+x, col);
        if (in_bounds(cx+y, cy-x)) pset_fast(cx+y, cy-x, col);
        if (in_bounds(cx-y, cy-x)) pset_fast(cx-y, cy-x, col);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

static void hline(int x0, int x1, int y, uint8_t col) {
    if ((unsigned)y >= GFX_HEIGHT) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    x0 = clip(x0, 0, GFX_WIDTH - 1);
    x1 = clip(x1, 0, GFX_WIDTH - 1);
    for (int x = x0; x <= x1; x++)
        pset_fast(x, y, col);
}

void gfx_circfill(int cx, int cy, int r, int c) {
    uint8_t col = c & 0xF;
    if (r < 0) return;
    int x = r, y = 0, d = 1 - r;
    while (x >= y) {
        hline(cx - x, cx + x, cy + y, col);
        hline(cx - x, cx + x, cy - y, col);
        hline(cx - y, cx + y, cy + x, col);
        hline(cx - y, cx + y, cy - x, col);
        y++;
        if (d < 0) {
            d += 2 * y + 1;
        } else {
            x--;
            d += 2 * (y - x) + 1;
        }
    }
}

// Decode a UTF-8 sequence starting at *str, return Unicode code point.
// Advances *str past the character (including variation selector U+FE0F).
// Returns 0 on error.
static uint32_t decode_utf8(const char **str) {
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t cp = 0;
    int nbytes = 0;
    unsigned char c = *s;

    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; nbytes = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; nbytes = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; nbytes = 4; }
    else { *str += 1; return 0; }

    for (int j = 1; j < nbytes; j++) {
        if ((s[j] & 0xC0) != 0x80) { *str += 1; return 0; }
        cp = (cp << 6) | (s[j] & 0x3F);
    }
    *str += nbytes;

    // Skip variation selector U+FE0F if present
    s = (const unsigned char *)*str;
    if (s[0] == 0xEF && s[1] == 0xB8 && s[2] == 0x8F)
        *str += 3;

    return cp;
}

static inline void draw_char_bg(int x, int y, int w, int bg_col) {
    for (int row = 0; row < 6; row++)
        for (int cx = 0; cx < w; cx++) {
            int px = x + cx, py = y + row;
            if (in_bounds(px, py)) pset_fast(px, py, bg_col);
        }
}

void gfx_print_w(const char *str, int x, int y, int c, int char_w) {
    uint8_t col = c & 0xF;
    int bg_col = -1; // -1 = no background, 0-15 = draw bg rect
    int ox = x;
    while (*str) {
        unsigned char ch = (unsigned char)*str;

        // P8SCII control codes (bytes 1-15, excluding newline 10)
        if (ch >= 1 && ch <= 15 && ch != '\n') {
            col = ch & 0xF;
            str++;
            continue;
        }

        if (ch == '\n') {
            str++;
            x = ox;
            y += 6;
            continue;
        }

        // P8SCII command prefix (byte 127): next byte is the command
        if (ch == 127) {
            str++;
            if (*str) {
                unsigned char cmd = (unsigned char)*str;
                str++;
                switch (cmd) {
                    case 'h': x = ox; y = 0; break;          // home
                    case 'i': x = (x / (char_w * 4) + 1) * (char_w * 4); break; // tab
                    case 'j': x = ox; y += 6; break;          // linefeed
                    case 'k': bg_col = -1; break;              // decoration off
                    case 'l': x = ox; break;                   // start of line (same as CR)
                    case 'm': x = ox; break;                   // carriage return
                    case 'o': break;                           // decoration on (uses current d)
                    case '-': x += char_w; break;              // skip right
                    case '+': y += 6; break;                   // skip down
                    case '#': x += char_w * 2; break;          // skip right (wide)
                    case 'x': if (*str) x = (unsigned char)*str++; break; // set X
                    case 'y': if (*str) y = (unsigned char)*str++; break; // set Y
                    case 'c': if (*str) str++; break;          // cursor char (consume)
                    case 'd': if (*str) { unsigned char dv = (unsigned char)*str++; bg_col = dv ? (dv & 0xF) : -1; } break;
                    case 's': if (*str) { p8_sfx_play((unsigned char)*str++, -1, 0, 32); } break;
                    case 'w': if (*str) str++; break;          // delay (consume, no-op)
                    // Commands with param byte — consume to keep stream aligned
                    case 'g': case 'n': case 'p': case 'q':
                    case 'r': case 't': case 'u': case 'v':
                        if (*str) str++;
                        break;
                    default: break;
                }
            }
            continue;
        }

        // P8SCII special display chars (16-31) — render as 4px glyphs
        if (ch >= 16 && ch <= 31) {
            str++;
            if (bg_col >= 0) draw_char_bg(x, y, char_w, bg_col);
            const uint8_t *glyph = font_p8_special[ch - 16];
            for (int row = 0; row < 6; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 4; col_idx++) {
                    if (bits & (1 << col_idx)) {
                        int px = x + col_idx;
                        int py = y + row;
                        if (in_bounds(px, py))
                            pset_fast(px, py, col);
                    }
                }
            }
            x += char_w;
            continue;
        }

        // UTF-8 multi-byte → decode and look up P8SCII glyph
        if (ch >= 0xC0) {
            uint32_t cp = decode_utf8(&str);
            int p8 = (cp > 0) ? unicode_to_p8scii(cp) : -1;
            if (p8 >= 128) {
                // Wide P8SCII character (8px)
                if (bg_col >= 0) draw_char_bg(x, y, char_w * 2, bg_col);
                const uint8_t *glyph = font_p8_wide[p8 - 128];
                for (int row = 0; row < 6; row++) {
                    uint8_t bits = glyph[row];
                    for (int col_idx = 0; col_idx < 8; col_idx++) {
                        if (bits & (0x80 >> col_idx)) {
                            int px = x + col_idx;
                            int py = y + row;
                            if (in_bounds(px, py))
                                pset_fast(px, py, col);
                        }
                    }
                }
                x += char_w * 2; // double width
            } else if (p8 >= 16 && p8 <= 31) {
                // Special display char (4px)
                if (bg_col >= 0) draw_char_bg(x, y, char_w, bg_col);
                const uint8_t *glyph = font_p8_special[p8 - 16];
                for (int row = 0; row < 6; row++) {
                    uint8_t bits = glyph[row];
                    for (int col_idx = 0; col_idx < 4; col_idx++) {
                        if (bits & (1 << col_idx)) {
                            int px = x + col_idx;
                            int py = y + row;
                            if (in_bounds(px, py))
                                pset_fast(px, py, col);
                        }
                    }
                }
                x += char_w;
            } else {
                // Unknown → render '?'
                if (bg_col >= 0) draw_char_bg(x, y, char_w, bg_col);
                const uint8_t *glyph = font_4x6['?' - 32];
                for (int row = 0; row < 6; row++) {
                    uint8_t bits = glyph[row];
                    for (int col_idx = 0; col_idx < 4; col_idx++) {
                        if (bits & (1 << col_idx)) {
                            int px = x + col_idx;
                            int py = y + row;
                            if (in_bounds(px, py))
                                pset_fast(px, py, col);
                        }
                    }
                }
                x += char_w;
            }
            continue;
        }

        // Raw P8SCII byte 128-255 (single-byte, not UTF-8)
        if (ch >= 128) {
            str++;
            if (bg_col >= 0) draw_char_bg(x, y, char_w * 2, bg_col);
            const uint8_t *glyph = font_p8_wide[ch - 128];
            for (int row = 0; row < 6; row++) {
                uint8_t bits = glyph[row];
                for (int col_idx = 0; col_idx < 8; col_idx++) {
                    if (bits & (0x80 >> col_idx)) {
                        int px = x + col_idx;
                        int py = y + row;
                        if (in_bounds(px, py))
                            pset_fast(px, py, col);
                    }
                }
            }
            x += char_w * 2;
            continue;
        }

        str++;
        // ASCII range
        if (ch < 32 || ch > 126) ch = '?';
        if (bg_col >= 0) draw_char_bg(x, y, char_w, bg_col);
        const uint8_t *glyph = font_4x6[ch - 32];
        for (int row = 0; row < 6; row++) {
            uint8_t bits = glyph[row];
            for (int col_idx = 0; col_idx < 4; col_idx++) {
                if (bits & (1 << col_idx)) {
                    int px = x + col_idx;
                    int py = y + row;
                    if (in_bounds(px, py))
                        pset_fast(px, py, col);
                }
            }
        }
        x += char_w;
    }
}

void gfx_print(const char *str, int x, int y, int c) {
    gfx_print_w(str, x, y, c, 4);
}

int gfx_text_width(const char *str, int char_w) {
    int w = 0;
    while (*str) {
        unsigned char ch = (unsigned char)*str;
        if (ch >= 1 && ch <= 15 && ch != '\n') { str++; continue; }
        if (ch == '\n') break;
        // P8SCII command prefix: handle cursor movement for width calc
        if (ch == 127) {
            str++;
            if (*str) {
                unsigned char cmd = (unsigned char)*str;
                str++;
                switch (cmd) {
                    case '-': w += char_w; break;
                    case 'i': w = (w / (char_w * 4) + 1) * (char_w * 4); break;
                    case '+': case 'k': case 'o': break;
                    case '#': w += char_w * 2; break;
                    case 'h': case 'j': case 'l': case 'm': w = 0; break;
                    case 'x': if (*str) { w = (unsigned char)*str++; } break;
                    case 'y': if (*str) str++; break;
                    case 'c': case 'd': case 's': case 'w':
                    case 'g': case 'n': case 'p': case 'q':
                    case 'r': case 't': case 'u': case 'v':
                        if (*str) str++;
                        break;
                    default: break;
                }
            }
            continue;
        }
        // P8SCII special display chars (16-31)
        if (ch >= 16 && ch <= 31) { str++; w += char_w; continue; }
        if (ch >= 0xC0) {
            uint32_t cp = decode_utf8(&str);
            int p8 = (cp > 0) ? unicode_to_p8scii(cp) : -1;
            if (p8 >= 128)
                w += char_w * 2;
            else
                w += char_w;
            continue;
        }
        if (ch >= 128) { str++; w += char_w * 2; continue; }
        str++;
        w += char_w;
    }
    return w;
}

void gfx_flip(void) {
    // Convert 4-bit indexed framebuffer to RGB565 via palette LUT
    // PICO-8 nibble order: low nibble = left (even x), high nibble = right (odd x)
    // Apply display palette if set (screen-level color remapping for fade/flash effects)
    if (gfx_disp_pal) {
        for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT / 2; i++) {
            uint8_t pair = fb4[i];
            fb_dvi[i * 2    ] = palette[gfx_disp_pal[pair & 0xF]];
            fb_dvi[i * 2 + 1] = palette[gfx_disp_pal[(pair >> 4) & 0xF]];
        }
    } else {
        for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT / 2; i++) {
            uint8_t pair = fb4[i];
            fb_dvi[i * 2    ] = palette[pair & 0xF];
            fb_dvi[i * 2 + 1] = palette[(pair >> 4) & 0xF];
        }
    }
}

void gfx_set_fb(uint8_t *buf) {
    fb4 = buf ? buf : fb4_default;
}

uint8_t *gfx_get_fb(void) {
    return fb4;
}

void gfx_set_display_pal(const uint8_t *pal) {
    gfx_disp_pal = pal;
}

// --- Lua bindings ---

static int l_cls(lua_State *L) {
    int c = (int)luaL_optinteger(L, 1, 0);
    gfx_cls(c);
    return 0;
}

static int l_pset(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int c = (int)luaL_optinteger(L, 3, draw_color);
    gfx_pset(x, y, c);
    return 0;
}

static int l_pget(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, gfx_pget(x, y));
    return 1;
}

static int l_rect(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    int c  = (int)luaL_optinteger(L, 5, draw_color);
    gfx_rect(x0, y0, x1, y1, c);
    return 0;
}

static int l_rectfill(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    int c  = (int)luaL_optinteger(L, 5, draw_color);
    gfx_rectfill(x0, y0, x1, y1, c);
    return 0;
}

static int l_line(lua_State *L) {
    int x0 = (int)luaL_checkinteger(L, 1);
    int y0 = (int)luaL_checkinteger(L, 2);
    int x1 = (int)luaL_checkinteger(L, 3);
    int y1 = (int)luaL_checkinteger(L, 4);
    int c  = (int)luaL_optinteger(L, 5, draw_color);
    gfx_line(x0, y0, x1, y1, c);
    return 0;
}

static int l_circ(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int r = (int)luaL_checkinteger(L, 3);
    int c = (int)luaL_optinteger(L, 4, draw_color);
    gfx_circ(x, y, r, c);
    return 0;
}

static int l_circfill(lua_State *L) {
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int r = (int)luaL_checkinteger(L, 3);
    int c = (int)luaL_optinteger(L, 4, draw_color);
    gfx_circfill(x, y, r, c);
    return 0;
}

static int l_print(lua_State *L) {
    const char *str = luaL_checkstring(L, 1);
    int x = (int)luaL_optinteger(L, 2, 0);
    int y = (int)luaL_optinteger(L, 3, 0);
    int c = (int)luaL_optinteger(L, 4, draw_color);
    gfx_print(str, x, y, c);
    return 0;
}

static int l_flip(lua_State *L) {
    (void)L;
    gfx_flip();
    return 0;
}

static int l_color(lua_State *L) {
    draw_color = (int)luaL_checkinteger(L, 1) & 0xF;
    return 0;
}

static const luaL_Reg gfxlib[] = {
    {"cls",      l_cls},
    {"pset",     l_pset},
    {"pget",     l_pget},
    {"rect",     l_rect},
    {"rectfill", l_rectfill},
    {"line",     l_line},
    {"circ",     l_circ},
    {"circfill", l_circfill},
    {"print",    l_print},
    {"flip",     l_flip},
    {"color",    l_color},
    {NULL, NULL}
};

int luaopen_gfx(lua_State *L) {
    luaL_newlib(L, gfxlib);
    return 1;
}
