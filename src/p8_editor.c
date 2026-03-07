/*
 * PICO-8 On-Device Code Editor
 *
 * Full-screen text editor on the 128x128 DVI display using 4x6 font.
 * Text buffer in PSRAM via TLSF. Saves/loads .p8 cartridge files.
 *
 * ESC toggles between REPL and editor (state persists).
 * Save from REPL with: save "filename"
 */

#include "p8_editor.h"
#include "p8_cart.h"
#include "p8_console.h"
#include "gfx.h"
#include "input.h"
#include "fatfs/ff.h"
#include "tlsf/tlsf.h"
#include "tusb.h"
#include "pico/stdlib.h"

#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <stdio.h>

// Display layout
#define CHAR_W      4
#define CHAR_H      6
#define SCREEN_COLS (128 / CHAR_W)  // 32
#define VIS_ROWS    19              // code lines visible (y=7..y=120)
#define CODE_Y      7
#define STATUS_Y    122

// Text limits
#define TEXT_MAX    65535
#define TEXT_INIT   4096

// Line cache
#define LINE_CACHE_MAX 8192

// HID keycodes
#define K_ESC    0x29
#define K_TAB    0x2B
#define K_DEL    0x4C
#define K_RIGHT  0x4F
#define K_LEFT   0x50
#define K_DOWN   0x51
#define K_UP     0x52
#define K_HOME   0x4A
#define K_END    0x4D
#define K_PGUP   0x4B
#define K_PGDN   0x4E
#define K_LCTRL  0xE0
#define K_RCTRL  0xE4
#define K_LSHIFT 0xE1
#define K_RSHIFT 0xE5
#define K_C      0x06
#define K_V      0x19
#define K_X      0x1B
#define K_A      0x04

static tlsf_t ed_tlsf;

// Persistent state — survives across editor open/close cycles
static bool ed_allocated = false;

// Text buffer (PSRAM)
static char *text;
static int text_len;
static int text_cap;

// Line offset cache (PSRAM) — O(1) line lookups
static int *line_cache;
static int line_cache_count;

// Cursor
static int cur_pos;     // byte offset in text
static int cur_row;     // 0-based line number
static int cur_col;     // 0-based column
static int scroll_y;    // first visible line
static int scroll_x;    // horizontal scroll offset
static int want_col;    // desired column for vertical movement

// File state
static char ed_path[256];
static bool ed_dirty;

// Preserved .p8 file sections (header + non-lua data)
static char *file_prefix;      // everything up to and including "__lua__\n"
static size_t prefix_len;
static char *file_suffix;      // everything from next __xxx__ section to EOF
static size_t suffix_len;

// Key repeat
static uint8_t khold[256];
static int blink_ctr;

// Selection state
static int sel_anchor;     // byte offset where selection started
static bool sel_active;    // true if a selection exists

// Clipboard
static char *clipboard;
static int clip_len;
static int clip_cap;

// ============================================================
// Key repeat helper
// ============================================================

static bool kp(uint8_t kc) {
    if (!input_key(kc)) { khold[kc] = 0; return false; }
    if (khold[kc] < 255) khold[kc]++;
    if (khold[kc] == 1) return true;        // initial press
    if (khold[kc] < 15) return false;        // delay
    return ((khold[kc] - 15) % 3 == 0);     // repeat
}

// ============================================================
// Line cache — rebuilt after each text edit
// ============================================================

static void cache_rebuild(void) {
    line_cache_count = 1;
    line_cache[0] = 0;
    for (int i = 0; i < text_len && line_cache_count < LINE_CACHE_MAX; i++) {
        if (text[i] == '\n')
            line_cache[line_cache_count++] = i + 1;
    }
}

static inline int lc_off(int row) {
    if (row < 0) return 0;
    if (row >= line_cache_count) return text_len;
    return line_cache[row];
}

static inline int lc_len(int row) {
    if (row < 0 || row >= line_cache_count) return 0;
    int s = line_cache[row];
    int e = (row + 1 < line_cache_count) ? line_cache[row + 1] - 1 : text_len;
    return e - s;
}

// ============================================================
// Cursor sync
// ============================================================

static void sync_rowcol(void) {
    cur_row = 0; cur_col = 0;
    for (int i = 0; i < cur_pos && i < text_len; i++) {
        if (text[i] == '\n') { cur_row++; cur_col = 0; }
        else cur_col++;
    }
}

static void sync_pos(void) {
    int s = lc_off(cur_row);
    int len = lc_len(cur_row);
    if (cur_col > len) cur_col = len;
    cur_pos = s + cur_col;
}

// ============================================================
// Text operations
// ============================================================

static bool grow(int need) {
    if (text_len + need <= text_cap) return true;
    int nc = text_cap * 2;
    while (nc < text_len + need) nc *= 2;
    if (nc > TEXT_MAX) nc = TEXT_MAX;
    if (text_len + need > nc) return false;
    char *nb = (char *)tlsf_realloc(ed_tlsf, text, nc);
    if (!nb) return false;
    text = nb; text_cap = nc;
    return true;
}

static void ins_ch(char c) {
    if (text_len >= TEXT_MAX || !grow(1)) return;
    memmove(text + cur_pos + 1, text + cur_pos, text_len - cur_pos);
    text[cur_pos++] = c;
    text_len++;
    ed_dirty = true;
    cache_rebuild();
    sync_rowcol();
    want_col = cur_col;
}

static void del_back(void) {
    if (cur_pos <= 0) return;
    cur_pos--;
    memmove(text + cur_pos, text + cur_pos + 1, text_len - cur_pos - 1);
    text_len--;
    ed_dirty = true;
    cache_rebuild();
    sync_rowcol();
    want_col = cur_col;
}

static void del_fwd(void) {
    if (cur_pos >= text_len) return;
    memmove(text + cur_pos, text + cur_pos + 1, text_len - cur_pos - 1);
    text_len--;
    ed_dirty = true;
    cache_rebuild();
}

// ============================================================
// Selection helpers
// ============================================================

static inline bool ctrl_held(void) {
    return input_key(K_LCTRL) || input_key(K_RCTRL);
}

static inline bool shift_held(void) {
    return input_key(K_LSHIFT) || input_key(K_RSHIFT);
}

static void sel_get(int *start, int *end) {
    if (!sel_active) { *start = *end = cur_pos; return; }
    if (sel_anchor <= cur_pos) { *start = sel_anchor; *end = cur_pos; }
    else { *start = cur_pos; *end = sel_anchor; }
}

// Begin or extend selection on shift; deselect otherwise
static void sel_update_before_move(void) {
    if (shift_held()) {
        if (!sel_active) { sel_anchor = cur_pos; sel_active = true; }
    } else {
        sel_active = false;
    }
}

// Delete selected text, leave cursor at selection start
static void sel_delete(void) {
    if (!sel_active) return;
    int s, e;
    sel_get(&s, &e);
    if (s == e) { sel_active = false; return; }
    memmove(text + s, text + e, text_len - e);
    text_len -= (e - s);
    cur_pos = s;
    sel_active = false;
    ed_dirty = true;
    cache_rebuild();
    sync_rowcol();
    want_col = cur_col;
}

// Copy selection to clipboard
static void clip_copy(void) {
    if (!sel_active) return;
    int s, e;
    sel_get(&s, &e);
    int len = e - s;
    if (len <= 0) return;
    if (len > clip_cap) {
        char *nb = (char *)tlsf_realloc(ed_tlsf, clipboard, len);
        if (!nb) nb = (char *)tlsf_malloc(ed_tlsf, len);
        if (!nb) return;
        clipboard = nb;
        clip_cap = len;
    }
    memcpy(clipboard, text + s, len);
    clip_len = len;
}

// Paste clipboard at cursor, replacing selection if any
static void clip_paste(void) {
    if (!clipboard || clip_len <= 0) return;
    if (sel_active) sel_delete();
    if (text_len + clip_len > TEXT_MAX || !grow(clip_len)) return;
    memmove(text + cur_pos + clip_len, text + cur_pos, text_len - cur_pos);
    memcpy(text + cur_pos, clipboard, clip_len);
    text_len += clip_len;
    cur_pos += clip_len;
    ed_dirty = true;
    cache_rebuild();
    sync_rowcol();
    want_col = cur_col;
}

// ============================================================
// Scrolling
// ============================================================

static void ensure_visible(void) {
    if (cur_row < scroll_y) scroll_y = cur_row;
    if (cur_row >= scroll_y + VIS_ROWS) scroll_y = cur_row - VIS_ROWS + 1;
    if (cur_col < scroll_x) scroll_x = cur_col;
    if (cur_col >= scroll_x + SCREEN_COLS) scroll_x = cur_col - SCREEN_COLS + 1;
}

// ============================================================
// Syntax highlighting
// ============================================================

// Color palette for highlighting
#define SYN_DEFAULT  6   // light grey
#define SYN_KEYWORD  12  // blue
#define SYN_NUMBER   10  // yellow
#define SYN_STRING   3   // dark green
#define SYN_COMMENT  5   // dark grey

// Check if s[pos..pos+klen) is a keyword (not followed by ident char)
static bool syn_keyword(const char *s, int pos, int len, const char *kw, int klen) {
    if (pos + klen > len) return false;
    if (memcmp(s + pos, kw, klen) != 0) return false;
    if (pos + klen < len) {
        char c = s[pos + klen];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') return false;
    }
    // Check char before
    if (pos > 0) {
        char c = s[pos - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') return false;
    }
    return true;
}

// Keywords table
static const struct { const char *w; int len; } lua_keywords[] = {
    {"if",2},{"then",4},{"else",4},{"elseif",6},{"end",3},
    {"function",8},{"local",5},{"return",6},{"for",3},{"while",5},
    {"do",2},{"repeat",6},{"until",5},{"in",2},{"not",3},
    {"and",3},{"or",2},{"true",4},{"false",5},{"nil",3},
    {"break",5},{"goto",4},
    {NULL,0}
};

// Colorize a line into colors[] array. Returns multiline comment state.
// in_long: true if we're inside a --[=*[ comment from a previous line
// long_level: bracket level of the long comment
static void colorize_line(const char *s, int len, uint8_t *colors,
                          bool *in_long, int *long_level) {
    int i = 0;

    // If we're inside a multi-line comment from a previous line
    if (*in_long) {
        while (i < len) {
            colors[i] = SYN_COMMENT;
            if (s[i] == ']') {
                int j = i + 1, cnt = 0;
                while (j < len && s[j] == '=') { cnt++; j++; }
                if (cnt == *long_level && j < len && s[j] == ']') {
                    for (int k = i; k <= j; k++) colors[k] = SYN_COMMENT;
                    i = j + 1;
                    *in_long = false;
                    break;
                }
            }
            i++;
        }
        if (*in_long) return; // entire line is comment
    }

    while (i < len) {
        char c = s[i];

        // Comments: -- to EOL (or --[=*[ for long comments)
        if (c == '-' && i + 1 < len && s[i + 1] == '-') {
            // Check for long comment --[=*[
            if (i + 2 < len && s[i + 2] == '[') {
                int j = i + 3, lvl = 0;
                while (j < len && s[j] == '=') { lvl++; j++; }
                if (j < len && s[j] == '[') {
                    // Long comment start
                    int start = i;
                    j++; // skip second [
                    // Search for closing ]=*]
                    while (j < len) {
                        if (s[j] == ']') {
                            int k = j + 1, cnt = 0;
                            while (k < len && s[k] == '=') { cnt++; k++; }
                            if (cnt == lvl && k < len && s[k] == ']') {
                                // Found closing — color everything
                                for (int m = start; m <= k; m++) colors[m] = SYN_COMMENT;
                                i = k + 1;
                                goto next_char;
                            }
                        }
                        j++;
                    }
                    // No closing on this line — multi-line comment
                    for (int m = start; m < len; m++) colors[m] = SYN_COMMENT;
                    *in_long = true;
                    *long_level = lvl;
                    return;
                }
            }
            // Regular line comment
            for (int j = i; j < len; j++) colors[j] = SYN_COMMENT;
            return;
        }
        // PICO-8 // comment
        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            for (int j = i; j < len; j++) colors[j] = SYN_COMMENT;
            return;
        }

        // String literals
        if (c == '"' || c == '\'') {
            colors[i] = SYN_STRING;
            char q = c;
            i++;
            while (i < len) {
                colors[i] = SYN_STRING;
                if (s[i] == '\\' && i + 1 < len) { i++; colors[i] = SYN_STRING; i++; continue; }
                if (s[i] == q) { i++; break; }
                i++;
            }
            continue;
        }

        // Numbers: 0x, 0b, digits, or digit starting with .
        if ((c >= '0' && c <= '9') ||
            (c == '.' && i + 1 < len && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            // Don't color if preceded by identifier char
            if (i > 0 && ((s[i-1] >= 'a' && s[i-1] <= 'z') || (s[i-1] >= 'A' && s[i-1] <= 'Z') || s[i-1] == '_')) {
                colors[i] = SYN_DEFAULT;
                i++;
                continue;
            }
            while (i < len && ((s[i] >= '0' && s[i] <= '9') ||
                               (s[i] >= 'a' && s[i] <= 'f') ||
                               (s[i] >= 'A' && s[i] <= 'F') ||
                               s[i] == 'x' || s[i] == 'X' ||
                               s[i] == 'b' || s[i] == 'B' ||
                               s[i] == '.')) {
                colors[i] = SYN_NUMBER;
                i++;
            }
            continue;
        }

        // Keywords
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            int start = i;
            bool found = false;
            for (int k = 0; lua_keywords[k].w; k++) {
                if (syn_keyword(s, i, len, lua_keywords[k].w, lua_keywords[k].len)) {
                    for (int j = 0; j < lua_keywords[k].len; j++)
                        colors[i + j] = SYN_KEYWORD;
                    i += lua_keywords[k].len;
                    found = true;
                    break;
                }
            }
            if (found) continue;
            // Regular identifier — skip past it
            while (i < len && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
                               (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
                colors[i] = SYN_DEFAULT;
                i++;
            }
            continue;
        }

        colors[i] = SYN_DEFAULT;
        i++;
        continue;
    next_char:;
    }
}

// ============================================================
// Drawing
// ============================================================

static bool hl_in_long = false;
static int hl_long_level = 0;

static void draw(void) {
    gfx_cls(1); // dark blue

    // --- Header bar ---
    gfx_rectfill(0, 0, 127, 5, 2);

    const char *name = ed_path;
    const char *sl = strrchr(ed_path, '/');
    if (sl) name = sl + 1;
    if (!name[0]) name = "[new]";

    char hdr[34];
    snprintf(hdr, sizeof(hdr), "%s%s", name, ed_dirty ? "*" : "");
    gfx_print_w(hdr, 1, 0, 7, CHAR_W);

    // Line:col on right side
    char pos_str[16];
    snprintf(pos_str, sizeof(pos_str), "%d:%d", cur_row + 1, cur_col + 1);
    int pw = (int)strlen(pos_str) * CHAR_W;
    gfx_print_w(pos_str, 128 - pw - 1, 0, 6, CHAR_W);

    // --- Code area with syntax highlighting ---
    // Determine multiline comment state at the scroll position.
    // Only scan lines that might start/end long comments (fast skip otherwise).
    hl_in_long = false;
    hl_long_level = 0;
    // Use a reusable buffer for pre-scan colorization (only state matters)
    uint8_t line_colors[256]; // enough for pre-scan; longer lines use heap
    for (int row = 0; row < scroll_y && row < line_cache_count; row++) {
        int s = lc_off(row);
        int ll = lc_len(row);
        if (!hl_in_long) {
            // Quick check: does line contain "--[" ?
            bool maybe = false;
            const char *lp = text + s;
            for (int j = 0; j + 2 < ll; j++) {
                if (lp[j] == '-' && lp[j+1] == '-' && lp[j+2] == '[') { maybe = true; break; }
            }
            if (!maybe) continue;
        }
        // Allocate temp buffer for lines longer than stack buffer
        uint8_t *cbuf = line_colors;
        uint8_t *heap_buf = NULL;
        if (ll > (int)sizeof(line_colors)) {
            heap_buf = (uint8_t *)tlsf_malloc(ed_tlsf, ll);
            cbuf = heap_buf ? heap_buf : line_colors;
        }
        if (ll > 0) {
            memset(cbuf, SYN_DEFAULT, ll > (int)sizeof(line_colors) ? (size_t)ll : sizeof(line_colors));
            colorize_line(text + s, ll, cbuf, &hl_in_long, &hl_long_level);
        }
        if (heap_buf) tlsf_free(ed_tlsf, heap_buf);
    }

    int total = line_cache_count;
    for (int i = 0; i < VIS_ROWS; i++) {
        int row = scroll_y + i;
        if (row >= total) break;
        int y = CODE_Y + i * CHAR_H;

        int s = lc_off(row);
        int len = lc_len(row);

        // Colorize the full line (for multiline state tracking)
        // Use a stack buffer sized to the line length
        uint8_t *full_colors = line_colors; // reuse if fits
        uint8_t *heap_colors = NULL;
        if (len > SCREEN_COLS) {
            heap_colors = (uint8_t *)tlsf_malloc(ed_tlsf, len + 1);
            full_colors = heap_colors ? heap_colors : line_colors;
        }
        memset(full_colors, SYN_DEFAULT, len > SCREEN_COLS ? (size_t)len : sizeof(line_colors));
        colorize_line(text + s, len, full_colors, &hl_in_long, &hl_long_level);

        // Visible portion
        int vs = scroll_x < len ? scroll_x : len;
        int vl = len - vs;
        if (vl > SCREEN_COLS) vl = SCREEN_COLS;

        // Determine selection range on this line
        int sel_s = 0, sel_e = 0;
        bool has_sel = false;
        if (sel_active) {
            int gs, ge;
            sel_get(&gs, &ge);
            // Line spans text[s..s+len), selection spans [gs..ge)
            int ls = s, le = s + len;
            if (gs < le && ge > ls) {
                sel_s = (gs > ls ? gs - ls : 0);
                sel_e = (ge < le ? ge - ls : len);
                has_sel = true;
            }
        }

        // Draw selection background
        if (has_sel) {
            int sx0 = sel_s - scroll_x;
            int sx1 = sel_e - scroll_x;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > SCREEN_COLS) sx1 = SCREEN_COLS;
            if (sx0 < sx1)
                gfx_rectfill(sx0 * CHAR_W, y, sx1 * CHAR_W - 1, y + CHAR_H - 1, 12);
        }

        // Render colored spans
        if (vl > 0) {
            int j = 0;
            while (j < vl) {
                bool in_sel = has_sel && (vs + j) >= sel_s && (vs + j) < sel_e;
                uint8_t col = in_sel ? 7 : full_colors[vs + j];
                int span_start = j;
                while (j < vl) {
                    bool j_sel = has_sel && (vs + j) >= sel_s && (vs + j) < sel_e;
                    if (j_sel != in_sel) break;
                    if (!in_sel && full_colors[vs + j] != col) break;
                    j++;
                }
                // Render span
                char lb[SCREEN_COLS + 1];
                int span_len = j - span_start;
                memcpy(lb, text + s + vs + span_start, span_len);
                for (int k = 0; k < span_len; k++)
                    if (lb[k] == '\t') lb[k] = ' ';
                lb[span_len] = '\0';
                gfx_print_w(lb, span_start * CHAR_W, y, col, CHAR_W);
            }
        }

        if (heap_colors) tlsf_free(ed_tlsf, heap_colors);

        // Cursor on this line (only blink when no selection)
        if (row == cur_row && !sel_active) {
            int cx = (cur_col - scroll_x) * CHAR_W;
            if (cx >= 0 && cx < 128) {
                blink_ctr++;
                if (blink_ctr & 16) {
                    gfx_rectfill(cx, y, cx + CHAR_W - 1, y + CHAR_H - 1, 7);
                    if (cur_pos < text_len && text[cur_pos] != '\n') {
                        char ch[2] = { text[cur_pos], '\0' };
                        gfx_print_w(ch, cx, y, 1, CHAR_W);
                    }
                }
            }
        }
    }

    // --- Status bar ---
    gfx_rectfill(0, 121, 127, 127, 2);
    gfx_print_w("esc:terminal", 1, STATUS_Y, 6, CHAR_W);

    gfx_flip();
}

// ============================================================
// File I/O
// ============================================================

static void free_sections(void) {
    if (file_prefix) { tlsf_free(ed_tlsf, file_prefix); file_prefix = NULL; }
    if (file_suffix) { tlsf_free(ed_tlsf, file_suffix); file_suffix = NULL; }
    prefix_len = suffix_len = 0;
}

static bool load_file(const char *path) {
    static FIL fil;  // static to avoid ~600 bytes on stack
    if (f_open(&fil, path, FA_READ) != FR_OK) return false;

    FSIZE_t fsize = f_size(&fil);

    // Allow reading large .p8 files (100KB+ with sprite data) —
    // we only keep the __lua__ section which is much smaller
    char *tmp = (char *)tlsf_malloc(ed_tlsf, (size_t)fsize + 1);
    if (!tmp) { f_close(&fil); return false; }

    UINT br;
    f_read(&fil, tmp, (UINT)fsize, &br);
    f_close(&fil);
    tmp[br] = '\0';

    // Strip \r and convert tabs to spaces (both show as '?' in renderer)
    {
        int w = 0;
        for (int r = 0; r < (int)br; r++) {
            if (tmp[r] == '\r') continue;
            if (tmp[r] == '\t') { tmp[w++] = ' '; continue; }
            tmp[w++] = tmp[r];
        }
        br = (UINT)w;
        tmp[br] = '\0';
    }

    free_sections();

    // Detect .p8 format
    size_t plen = strlen(path);
    bool is_p8 = (plen >= 3 && strcmp(path + plen - 3, ".p8") == 0);

    if (is_p8) {
        // Find __lua__ header
        const char *lua_hdr = NULL;
        const char *p = tmp;
        const char *end = tmp + br;

        while (p < end) {
            if ((p == tmp || *(p - 1) == '\n') &&
                (size_t)(end - p) >= 7 && memcmp(p, "__lua__", 7) == 0) {
                lua_hdr = p;
                break;
            }
            while (p < end && *p != '\n') p++;
            if (p < end) p++;
        }

        if (lua_hdr) {
            // Skip to end of __lua__ header line
            const char *line_end = lua_hdr + 7;
            while (line_end < end && *line_end != '\n') line_end++;
            if (line_end < end) line_end++; // include \n

            // Save prefix (header through __lua__\n)
            prefix_len = line_end - tmp;
            file_prefix = (char *)tlsf_malloc(ed_tlsf, prefix_len);
            if (file_prefix) memcpy(file_prefix, tmp, prefix_len);

            // Find end of lua section (next __xxx__ header or EOF)
            const char *lua_start = line_end;
            const char *lua_end = lua_start;
            while (lua_end < end) {
                if ((lua_end == lua_start || *(lua_end - 1) == '\n') &&
                    lua_end + 1 < end && *lua_end == '_' && *(lua_end + 1) == '_') {
                    break;
                }
                lua_end++;
            }

            // Save suffix (remaining sections)
            if (lua_end < end) {
                suffix_len = end - lua_end;
                file_suffix = (char *)tlsf_malloc(ed_tlsf, suffix_len);
                if (file_suffix) memcpy(file_suffix, lua_end, suffix_len);
            }

            // Trim trailing newlines from lua content for editing
            const char *lua_trim = lua_end;
            while (lua_trim > lua_start && *(lua_trim - 1) == '\n') lua_trim--;

            size_t lua_len = lua_trim - lua_start;
            if ((int)lua_len >= text_cap) {
                int nc = (int)lua_len + 1024;
                if (nc > TEXT_MAX) nc = TEXT_MAX;
                char *nb = (char *)tlsf_realloc(ed_tlsf, text, nc);
                if (nb) { text = nb; text_cap = nc; }
            }
            text_len = (int)lua_len < text_cap ? (int)lua_len : text_cap;
            memcpy(text, lua_start, text_len);
        } else {
            // No __lua__ found, load as plain text
            if ((int)br >= text_cap) {
                int nc = (int)br + 1024;
                if (nc > TEXT_MAX) nc = TEXT_MAX;
                char *nb = (char *)tlsf_realloc(ed_tlsf, text, nc);
                if (nb) { text = nb; text_cap = nc; }
            }
            text_len = (int)br < text_cap ? (int)br : text_cap;
            memcpy(text, tmp, text_len);
        }
    } else {
        // Plain text file
        if ((int)br >= text_cap) {
            int nc = (int)br + 1024;
            if (nc > TEXT_MAX) nc = TEXT_MAX;
            char *nb = (char *)tlsf_realloc(ed_tlsf, text, nc);
            if (nb) { text = nb; text_cap = nc; }
        }
        text_len = (int)br < text_cap ? (int)br : text_cap;
        memcpy(text, tmp, text_len);
    }

    tlsf_free(ed_tlsf, tmp);

    snprintf(ed_path, sizeof(ed_path), "%s", path);
    cur_pos = cur_row = cur_col = scroll_y = scroll_x = want_col = 0;
    ed_dirty = false;
    cache_rebuild();
    return true;
}

// Safe write helper — checks return value and byte count
static bool safe_write(FIL *fil, const void *data, UINT len) {
    if (len == 0) return true;
    UINT bw;
    FRESULT res = f_write(fil, data, len, &bw);
    if (res != FR_OK || bw != len) {
        printf("  f_write failed: res=%d, wrote %u/%u\n", res, (unsigned)bw, (unsigned)len);
        return false;
    }
    return true;
}

static bool save_file(void) {
    if (!ed_path[0]) return false;

    // Write to a temp file first — never truncate the original until
    // the new data is fully committed to disk.
    static char tmp_path[260];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ed_path);

    static FIL fil;  // static to avoid ~600 bytes on stack (Lua call chain is deep)
    FRESULT fres = f_open(&fil, tmp_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fres != FR_OK) {
        printf("save: f_open failed: %d\n", fres);
        return false;
    }

    bool ok = true;
    size_t plen = strlen(ed_path);
    bool is_p8 = (plen >= 3 && strcmp(ed_path + plen - 3, ".p8") == 0);

    if (is_p8) {
        // Write header
        if (file_prefix && prefix_len > 0) {
            ok = safe_write(&fil, file_prefix, (UINT)prefix_len);
        } else {
            const char *hdr =
                "pico-8 cartridge // http://www.pico-8.com\n"
                "version 41\n"
                "__lua__\n";
            ok = safe_write(&fil, hdr, (UINT)strlen(hdr));
        }

        // Write edited lua code
        if (ok && text_len > 0)
            ok = safe_write(&fil, text, (UINT)text_len);

        // Ensure newline before next section
        if (ok && (text_len == 0 || text[text_len - 1] != '\n'))
            ok = safe_write(&fil, "\n", 1);

        // Write preserved data sections (__gfx__, __map__, etc.)
        if (ok && file_suffix && suffix_len > 0)
            ok = safe_write(&fil, file_suffix, (UINT)suffix_len);
    } else {
        // Plain text
        if (text_len > 0)
            ok = safe_write(&fil, text, (UINT)text_len);
    }

    if (ok) {
        // Flush all data to SD card before closing
        fres = f_sync(&fil);
        if (fres != FR_OK) {
            printf("save: f_sync failed: %d\n", fres);
            ok = false;
        }
    }

    f_close(&fil);

    if (!ok) {
        // Write failed — delete the temp file, original is untouched
        f_unlink(tmp_path);
        printf("save: aborted, original file preserved\n");
        return false;
    }

    // Success — atomically replace original with temp file
    f_unlink(ed_path);     // remove original (ok if it doesn't exist)
    fres = f_rename(tmp_path, ed_path);
    if (fres != FR_OK) {
        printf("save: rename failed: %d (data in %s)\n", fres, tmp_path);
        return false;
    }

    ed_dirty = false;
    return true;
}

// ============================================================
// Ensure buffers are allocated (lazy init)
// ============================================================

static bool ensure_allocated(void) {
    if (ed_allocated) return true;

    text_cap = TEXT_INIT;
    text = (char *)tlsf_malloc(ed_tlsf, text_cap);
    line_cache = (int *)tlsf_malloc(ed_tlsf, LINE_CACHE_MAX * sizeof(int));
    if (!text || !line_cache) {
        printf("editor: out of memory\n");
        if (text) { tlsf_free(ed_tlsf, text); text = NULL; }
        if (line_cache) { tlsf_free(ed_tlsf, line_cache); line_cache = NULL; }
        return false;
    }

    text_len = 0;
    cur_pos = cur_row = cur_col = scroll_y = scroll_x = want_col = 0;
    ed_dirty = false;
    ed_path[0] = '\0';
    file_prefix = NULL; prefix_len = 0;
    file_suffix = NULL; suffix_len = 0;
    sel_active = false; sel_anchor = 0;
    clipboard = NULL; clip_len = 0; clip_cap = 0;

    line_cache_count = 1;
    line_cache[0] = 0;

    ed_allocated = true;
    return true;
}

// ============================================================
// Public API
// ============================================================

void p8_editor_init(tlsf_t tlsf) {
    ed_tlsf = tlsf;
}

void p8_editor_enter(void) {
    if (!ensure_allocated()) return;

    // If editor buffer is empty and a cart is loaded, auto-load its code
    if (text_len == 0 && ed_path[0] == '\0') {
        const char *cart = p8_cart_get_path();
        if (cart && cart[0])
            load_file(cart);
    }

    input_flush();
    memset(khold, 0, sizeof(khold));
    blink_ctr = 0;
    sel_active = false;

    // Wait for ESC release (debounce from REPL toggle)
    while (input_key(K_ESC)) { tuh_task(); input_update(); sleep_ms(10); }

    printf("editor: %s (%d bytes)\n", ed_path[0] ? ed_path : "[new]", text_len);

    // ====== Main editor loop ======
    while (true) {
        tuh_task();
        input_update();

        // ESC: return to REPL (keep state)
        if (kp(K_ESC)) break;

        bool ctrl = ctrl_held();

        // Ctrl+A: select all
        if (ctrl && kp(K_A)) {
            sel_anchor = 0;
            cur_pos = text_len;
            sel_active = true;
            sync_rowcol();
            want_col = cur_col;
        }

        // Ctrl+C: copy
        if (ctrl && kp(K_C)) {
            clip_copy();
        }

        // Ctrl+X: cut
        if (ctrl && kp(K_X)) {
            clip_copy();
            sel_delete();
        }

        // Ctrl+V: paste
        if (ctrl && kp(K_V)) {
            clip_paste();
        }

        // Arrow keys (with shift for selection)
        if (kp(K_LEFT)) {
            sel_update_before_move();
            if (cur_pos > 0) {
                cur_pos--;
                sync_rowcol();
                want_col = cur_col;
            }
        }
        if (kp(K_RIGHT)) {
            sel_update_before_move();
            if (cur_pos < text_len) {
                cur_pos++;
                sync_rowcol();
                want_col = cur_col;
            }
        }
        if (kp(K_UP)) {
            sel_update_before_move();
            if (cur_row > 0) {
                cur_row--;
                cur_col = want_col;
                int ll = lc_len(cur_row);
                if (cur_col > ll) cur_col = ll;
                sync_pos();
            }
        }
        if (kp(K_DOWN)) {
            sel_update_before_move();
            if (cur_row < line_cache_count - 1) {
                cur_row++;
                cur_col = want_col;
                int ll = lc_len(cur_row);
                if (cur_col > ll) cur_col = ll;
                sync_pos();
            }
        }

        // Home / End (with shift for selection)
        if (kp(K_HOME)) { sel_update_before_move(); cur_col = 0; sync_pos(); want_col = 0; }
        if (kp(K_END))  { sel_update_before_move(); cur_col = lc_len(cur_row); sync_pos(); want_col = cur_col; }

        // Page Up / Down (with shift for selection)
        if (kp(K_PGUP)) {
            sel_update_before_move();
            cur_row -= VIS_ROWS;
            if (cur_row < 0) cur_row = 0;
            cur_col = want_col;
            int ll = lc_len(cur_row);
            if (cur_col > ll) cur_col = ll;
            sync_pos();
        }
        if (kp(K_PGDN)) {
            sel_update_before_move();
            cur_row += VIS_ROWS;
            if (cur_row >= line_cache_count) cur_row = line_cache_count - 1;
            cur_col = want_col;
            int ll = lc_len(cur_row);
            if (cur_col > ll) cur_col = ll;
            sync_pos();
        }

        // Tab: insert 2 spaces (replaces selection)
        if (kp(K_TAB)) {
            if (sel_active) sel_delete();
            ins_ch(' ');
            ins_ch(' ');
        }

        // Delete (forward) — deletes selection if active
        if (kp(K_DEL)) {
            if (sel_active) sel_delete();
            else del_fwd();
        }

        // Character input from keyboard buffer (printable, enter, backspace)
        int ch;
        while ((ch = input_getchar()) >= 0) {
            if (ch == '\n') {
                if (sel_active) sel_delete();
                ins_ch('\n');
            } else if (ch == '\b') {
                if (sel_active) sel_delete();
                else del_back();
            } else if ((ch >= 32 && ch < 127) || ch >= 128) {
                if (sel_active) sel_delete();
                ins_ch((char)ch);
            }
        }

        ensure_visible();
        draw();
        sleep_ms(16); // ~60fps
    }

    input_flush();
    printf("editor closed\n");
    // Note: buffers are NOT freed — state persists for re-entry
}

bool p8_editor_load(const char *path) {
    if (!ensure_allocated()) return false;
    return load_file(path);
}

bool p8_editor_save(const char *path) {
    if (!ensure_allocated()) return false;
    if (!text) return false;

    // Set path if provided
    if (path && path[0]) {
        snprintf(ed_path, sizeof(ed_path), "%s", path);
        // Add .p8 extension if missing
        size_t pl = strlen(ed_path);
        if (pl < 3 || strcmp(ed_path + pl - 3, ".p8") != 0) {
            if (pl + 3 < sizeof(ed_path))
                strcat(ed_path, ".p8");
        }
    }

    return save_file();
}

// ============================================================
// Lua bindings
// ============================================================

// edit([path]) — load file (if given) and enter editor
static int lua_edit(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");

    if (path[0] != '\0')
        p8_editor_load(path);

    p8_editor_enter();

    // Restore console display after editor exits
    p8_console_draw();
    gfx_flip();
    return 0;
}

// save([path]) — save editor buffer to SD card
static int lua_save(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "");

    if (p8_editor_save(path)) {
        printf("saved: %s\n", ed_path);
        p8_console_printf("saved %s\n", ed_path);
    } else {
        if (!ed_path[0]) {
            printf("no filename. use: save \"filename\"\n");
            p8_console_print("no filename\n");
        } else {
            printf("save failed: %s\n", ed_path);
            p8_console_printf("save failed: %s\n", ed_path);
        }
    }
    p8_console_draw();
    gfx_flip();
    return 0;
}

void p8_editor_register(lua_State *L) {
    lua_register(L, "edit", lua_edit);
    lua_register(L, "save", lua_save);
}
