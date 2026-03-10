#include <cstdio>
#include <cstdarg>
#include <cstring>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "psram.h"
#include "pio_usb.h"
#include "usb-host/fwUSBHost.h"

extern "C" {
#include "tusb.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "tlsf/tlsf.h"
#include "fatfs/ff.h"
#include "sdcard.h"
#include "gfx.h"
#include "dvi.h"
#include "input.h"
#include "audio.h"
#include "p8_preprocess.h"
#include "p8_api.h"
#include "p8_sfx.h"
#include "p8_cart.h"
#include "p8_console.h"
#include "p8_editor.h"
}

// TinyUSB debug printf (declared in tusb_config.h)
extern "C" int cdc_debug_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

// TinyUSB debug ring buffer — captures debug output, dumped via 'info'
static constexpr int TUSB_DBG_BUF_SIZE = 4096;
static char tusb_dbg_buf[TUSB_DBG_BUF_SIZE];
static int tusb_dbg_pos = 0;
static bool tusb_dbg_wrapped = false;

extern "C" int tusb_debug_buffered_printf(const char* fmt, ...) {
    char tmp[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    if (n <= 0) return n;
    if (n > (int)sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    // Filter out device-side (USBD/CDC) noise — only keep host-side logs
    if (strncmp(tmp, "USBD", 4) == 0 || strncmp(tmp, "  CDC", 5) == 0 ||
        strncmp(tmp, "  Queue", 7) == 0)
        return n;
    for (int i = 0; i < n; i++) {
        tusb_dbg_buf[tusb_dbg_pos] = tmp[i];
        tusb_dbg_pos = (tusb_dbg_pos + 1) % TUSB_DBG_BUF_SIZE;
        if (tusb_dbg_pos == 0) tusb_dbg_wrapped = true;
    }
    return n;
}

static void tusb_dbg_dump() {
    if (!tusb_dbg_wrapped && tusb_dbg_pos == 0) {
        printf("(no tusb debug output)\n");
        return;
    }
    printf("--- tusb debug log ---\n");
    if (tusb_dbg_wrapped) {
        // Print from current position to end, then start to current position
        fwrite(tusb_dbg_buf + tusb_dbg_pos, 1, TUSB_DBG_BUF_SIZE - tusb_dbg_pos, stdout);
        fwrite(tusb_dbg_buf, 1, tusb_dbg_pos, stdout);
    } else {
        fwrite(tusb_dbg_buf, 1, tusb_dbg_pos, stdout);
    }
    printf("--- end tusb debug ---\n");
}

static void tusb_dbg_clear() {
    tusb_dbg_pos = 0;
    tusb_dbg_wrapped = false;
}

extern fwUSBHost obUSBHost;

static tlsf_t psram_tlsf;
static size_t psram_total_size;
static FATFS fatfs;
static bool sd_mounted = false;

static void* lua_psram_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud; (void)osize;
    if (nsize == 0) {
        tlsf_free(psram_tlsf, ptr);
        return NULL;
    }
    if (ptr == NULL)
        return tlsf_malloc(psram_tlsf, nsize);
    return tlsf_realloc(psram_tlsf, ptr, nsize);
}

// Walk callback to sum up free space
struct mem_stats {
    size_t used;
    size_t free_bytes;
};

static void mem_walker(void* ptr, size_t size, int used, void* user) {
    (void)ptr;
    struct mem_stats* stats = (struct mem_stats*)user;
    if (used)
        stats->used += size;
    else
        stats->free_bytes += size;
}

// --- sys library ---
static int sys_memfree(lua_State *L) {
    struct mem_stats stats = {0, 0};
    pool_t pool = tlsf_get_pool(psram_tlsf);
    tlsf_walk_pool(pool, mem_walker, &stats);
    lua_pushinteger(L, (lua_Integer)stats.free_bytes);
    return 1;
}

static int sys_memused(lua_State *L) {
    struct mem_stats stats = {0, 0};
    pool_t pool = tlsf_get_pool(psram_tlsf);
    tlsf_walk_pool(pool, mem_walker, &stats);
    lua_pushinteger(L, (lua_Integer)stats.used);
    return 1;
}

static int sys_memtotal(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)psram_total_size);
    return 1;
}

static const luaL_Reg syslib[] = {
    {"memfree",  sys_memfree},
    {"memused",  sys_memused},
    {"memtotal", sys_memtotal},
    {NULL, NULL}
};

static int luaopen_sys(lua_State *L) {
    luaL_newlib(L, syslib);
    return 1;
}

// --- Helpers for loading files from SD ---

// Read an entire file from FatFS into a PSRAM-allocated buffer.
// Returns the buffer (caller must tlsf_free) and sets *out_size.
// Returns NULL on failure.
static char* read_file_to_psram(const char *path, size_t *out_size) {
    static FIL fil;  // static to avoid ~600 bytes on stack
    if (f_open(&fil, path, FA_READ) != FR_OK)
        return NULL;

    FSIZE_t fsize = f_size(&fil);
    if (fsize == 0) {
        f_close(&fil);
        *out_size = 0;
        // Return a 1-byte buffer for empty files
        char *buf = (char *)tlsf_malloc(psram_tlsf, 1);
        if (buf) buf[0] = '\0';
        return buf;
    }

    char *buf = (char *)tlsf_malloc(psram_tlsf, (size_t)fsize + 1);
    if (!buf) {
        f_close(&fil);
        return NULL;
    }

    UINT br;
    FRESULT res = f_read(&fil, buf, (UINT)fsize, &br);
    f_close(&fil);

    if (res != FR_OK || br != (UINT)fsize) {
        tlsf_free(psram_tlsf, buf);
        return NULL;
    }

    buf[br] = '\0';
    *out_size = (size_t)br;
    return buf;
}

// --- PICO-8 preprocessor integration ---

static bool is_p8_file(const char *path) {
    size_t len = strlen(path);
    return len >= 3 && strcmp(path + len - 3, ".p8") == 0;
}

// Load a file, optionally running it through the PICO-8 preprocessor.
// Returns preprocessed (or raw) buffer allocated via TLSF. Caller must free.
static char* load_and_preprocess(const char *path, size_t *out_size) {
    size_t sz;
    char *buf = read_file_to_psram(path, &sz);
    if (!buf) return NULL;

    if (is_p8_file(path)) {
        size_t pp_len;
        char *pp = p8_preprocess(buf, sz, &pp_len);
        tlsf_free(psram_tlsf, buf);
        if (!pp) return NULL;
        *out_size = pp_len;
        return pp;
    }

    *out_size = sz;
    return buf;
}

// --- Custom dofile/loadfile using FatFS ---

static int lua_custom_loadfile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    size_t sz;
    char *buf = load_and_preprocess(path, &sz);
    if (!buf) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", path);
        return 2;
    }

    // Prefix chunk name with '@' so errors show the filename
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);

    int status = luaL_loadbuffer(L, buf, sz, chunkname);
    tlsf_free(psram_tlsf, buf);

    if (status != LUA_OK) {
        // Error message is on stack
        lua_pushnil(L);
        lua_insert(L, -2); // nil, errmsg
        return 2;
    }
    return 1; // The compiled chunk
}

static int lua_custom_dofile(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    size_t sz;
    char *buf = load_and_preprocess(path, &sz);
    if (!buf)
        return luaL_error(L, "cannot open %s", path);

    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", path);

    int status = luaL_loadbuffer(L, buf, sz, chunkname);
    tlsf_free(psram_tlsf, buf);

    if (status != LUA_OK)
        return lua_error(L);

    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L) - 1; // Return all results (minus the path argument which was consumed)
}

// Custom searcher for require() — looks for files on SD card
static int lua_sd_searcher(lua_State *L) {
    const char *modname = luaL_checkstring(L, 1);

    // Get package.path
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    const char *path_template = lua_tostring(L, -1);
    lua_pop(L, 2);

    if (!path_template)
        return 1; // Return nil

    // Try each template in package.path (separated by ';')
    char trial[256];
    const char *p = path_template;
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t tlen = semi ? (size_t)(semi - p) : strlen(p);

        // Build path by replacing '?' with module name
        size_t ti = 0;
        for (size_t i = 0; i < tlen && ti < sizeof(trial) - 1; i++) {
            if (p[i] == '?') {
                size_t mlen = strlen(modname);
                if (ti + mlen < sizeof(trial) - 1) {
                    memcpy(trial + ti, modname, mlen);
                    ti += mlen;
                }
            } else {
                trial[ti++] = p[i];
            }
        }
        trial[ti] = '\0';

        // Try to open the file (preprocess .p8 files)
        size_t sz;
        char *buf = load_and_preprocess(trial, &sz);
        if (buf) {
            char chunkname[256];
            snprintf(chunkname, sizeof(chunkname), "@%s", trial);
            int status = luaL_loadbuffer(L, buf, sz, chunkname);
            tlsf_free(psram_tlsf, buf);
            if (status == LUA_OK) {
                lua_pushstring(L, trial); // 2nd return: file path
                return 2;
            }
            // Load error — return error message
            return lua_error(L);
        }

        if (!semi) break;
        p = semi + 1;
    }

    // Module not found
    lua_pushfstring(L, "\n\tno file on SD card");
    return 1;
}

// --- fs library ---

static int fs_list(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "/");
    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, path) != FR_OK)
        return luaL_error(L, "cannot open directory: %s", path);

    lua_newtable(L);
    int idx = 1;

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        lua_newtable(L);

        lua_pushstring(L, fno.fname);
        lua_setfield(L, -2, "name");

        lua_pushinteger(L, (lua_Integer)fno.fsize);
        lua_setfield(L, -2, "size");

        lua_pushboolean(L, (fno.fattrib & AM_DIR) != 0);
        lua_setfield(L, -2, "isdir");

        lua_rawseti(L, -2, idx++);
    }

    f_closedir(&dir);
    return 1;
}

static int fs_read(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);

    size_t sz;
    char *buf = read_file_to_psram(path, &sz);
    if (!buf)
        return luaL_error(L, "cannot read file: %s", path);

    lua_pushlstring(L, buf, sz);
    tlsf_free(psram_tlsf, buf);
    return 1;
}

static int fs_write(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    static FIL fil;  // static to avoid ~600 bytes on stack
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return luaL_error(L, "cannot open file for writing: %s", path);

    UINT bw;
    FRESULT res = f_write(&fil, data, (UINT)len, &bw);
    f_close(&fil);

    if (res != FR_OK || bw != (UINT)len)
        return luaL_error(L, "write error: %s", path);

    lua_pushboolean(L, 1);
    return 1;
}

static int fs_exists(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILINFO fno;
    lua_pushboolean(L, f_stat(path, &fno) == FR_OK);
    return 1;
}

static int fs_mkdir(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (f_mkdir(path) != FR_OK)
        return luaL_error(L, "cannot create directory: %s", path);
    lua_pushboolean(L, 1);
    return 1;
}

static int fs_remove(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (f_unlink(path) != FR_OK)
        return luaL_error(L, "cannot remove: %s", path);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg fslib[] = {
    {"list",    fs_list},
    {"read",    fs_read},
    {"write",   fs_write},
    {"exists",  fs_exists},
    {"mkdir",   fs_mkdir},
    {"remove",  fs_remove},
    {NULL, NULL}
};

static int luaopen_fs(lua_State *L) {
    luaL_newlib(L, fslib);
    return 1;
}

// --- Setup Lua overrides ---

// help command: PICO-8-style help with per-command detail
static void help_print(const char *s) {
    printf("%s\n", s);
    p8_console_printf("%s\n", s);
}

struct help_entry {
    const char *cmd;
    const char *brief;
    const char *detail[6]; // up to 6 lines of detail, NULL-terminated
};

static const struct help_entry help_entries[] = {
    {"load", "load <cart>",
     {"load a cartridge by name.", "also loads code into editor.",
      "  load mycart", "  load carts/jelpi", NULL}},
    {"save", "save <cart>",
     {"save editor code as .p8 cart.", "  save mycart", NULL}},
    {"run", "run cart",
     {"run the currently loaded cart.", "call load first.", NULL}},
    {"ls", "ls [dir]",
     {"list files in current or", "given directory.", "  ls", "  ls carts", NULL}},
    {"cd", "cd <dir>",
     {"change directory.", "  cd /", "  cd carts", "  cd ..", NULL}},
    {"mkdir", "mkdir <dir>",
     {"create a new directory.", "  mkdir carts", NULL}},
    {"rm", "rm <file>",
     {"delete a file. appends .p8", "if no exact match.", "  rm mycart", NULL}},
    {"cls", "cls",
     {"clear the screen.", NULL}},
    {"edit", "edit [cart]",
     {"open the code editor.", "esc toggles editor/terminal.", NULL}},
    {"reboot", "reboot",
     {"restart the system.", NULL}},
    {"info", "info",
     {"show system information.", NULL}},
    {"help", "help [cmd]",
     {"show this help, or help on", "a specific command.", NULL}},
    {NULL, NULL, {NULL}}
};

static int lua_help(lua_State *L) {
    const char *cmd = luaL_optstring(L, 1, NULL);

    if (cmd) {
        // help <command>
        for (int i = 0; help_entries[i].cmd; i++) {
            if (strcmp(cmd, help_entries[i].cmd) == 0) {
                help_print(help_entries[i].brief);
                for (int j = 0; help_entries[i].detail[j]; j++)
                    help_print(help_entries[i].detail[j]);
                p8_console_draw();
                gfx_flip();
                return 0;
            }
        }
        p8_console_printf("unknown command: %s\n", cmd);
        printf("unknown command: %s\n", cmd);
        p8_console_draw();
        gfx_flip();
        return 0;
    }

    // General help — PICO-8 style
    help_print("** wili8jam 0.10 **");
    help_print("");
    help_print("commands:");
    for (int i = 0; help_entries[i].cmd; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), " %-8s %s",
                 help_entries[i].cmd, help_entries[i].brief);
        // Truncate to fit 32 cols
        buf[32] = '\0';
        help_print(buf);
    }
    help_print("");
    help_print("help <cmd> for more info");

    p8_console_draw();
    gfx_flip();
    return 0;
}

// info command: display system info
static int lua_info(lua_State *L) {
    struct mem_stats stats = {0, 0};
    pool_t pool = tlsf_get_pool(psram_tlsf);
    tlsf_walk_pool(pool, mem_walker, &stats);
    unsigned fk = (unsigned)(stats.free_bytes / 1024);
    unsigned tk = (unsigned)(psram_total_size / 1024);
    unsigned mhz = (unsigned)(clock_get_hz(clk_sys) / 1000000);
    const char *fmt = "wili8jam / fruit jam\nrp2350b @ %u mhz\npsram: %uk free / %uk\n";
    printf(fmt, mhz, fk, tk);
    p8_console_printf(fmt, mhz, fk, tk);

    // USB HID device info
    auto &hid = obUSBHost.m_obHID;
    printf("usb: kbd=%d mouse=%d ctrl=%d xinput=%d generic=%d\n",
        hid.getKeyboardCount(), hid.getMouseCount(),
        hid.getControllerCount(), obUSBHost.m_obXInput.getMountedCount(),
        hid.getGenericCount());
    p8_console_printf("usb: kbd=%d mouse=%d ctrl=%d xinput=%d generic=%d\n",
        hid.getKeyboardCount(), hid.getMouseCount(),
        hid.getControllerCount(), obUSBHost.m_obXInput.getMountedCount(),
        hid.getGenericCount());

    // Show mounted HID interface details
    for (uint8_t addr = 1; addr <= CFG_TUH_DEVICE_MAX; addr++) {
        if (!tuh_mounted(addr)) continue;
        uint8_t itf_count = tuh_hid_itf_get_count(addr);
        for (uint8_t itf = 0; itf < itf_count; itf++) {
            uint8_t proto = tuh_hid_interface_protocol(addr, itf);
            printf("  dev %d itf %d: proto=%d (%s)\n", addr, itf, proto,
                proto == 1 ? "keyboard" : proto == 2 ? "mouse" : "none/other");
        }
    }

    // PIO-USB root port diagnostic
    {
        extern root_port_t pio_usb_root_port[];
        root_port_t *root = &pio_usb_root_port[0];
        printf("pio-usb: init=%d connected=%d fullspeed=%d suspended=%d event=%d\n",
            root->initialized, root->connected, root->is_fullspeed,
            root->suspended, root->event);
    }

    // Dump TinyUSB debug ring buffer
    tusb_dbg_dump();
    tusb_dbg_clear();

    p8_console_draw();
    gfx_flip();
    (void)L;
    return 0;
}

// reboot: restart the board
static int lua_reboot(lua_State *L) {
    (void)L;
    watchdog_reboot(0, 0, 0);
    while (1) tight_loop_contents();
    return 0;
}

// mkdir as global command
static int lua_mkdir(lua_State *L) { return fs_mkdir(L); }

// rm: try exact path first, then append .p8 if no match
static int lua_rm(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    if (f_unlink(path) == FR_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    // Try with .p8 extension
    char buf[256];
    snprintf(buf, sizeof(buf), "%s.p8", path);
    if (f_unlink(buf) == FR_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return luaL_error(L, "cannot remove: %s", path);
}

// cd: change current directory
static int lua_cd(lua_State *L) {
    const char *path = luaL_optstring(L, 1, "/");
    if (f_chdir(path) != FR_OK)
        return luaL_error(L, "cannot cd: %s", path);
    // Print new cwd
    char cwd[256];
    if (f_getcwd(cwd, sizeof(cwd)) == FR_OK) {
        printf("%s\n", cwd);
        p8_console_printf("%s\n", cwd);
    }
    p8_console_draw();
    gfx_flip();
    return 0;
}

// PICO-8-style ls command: lists files in current or given directory
static int lua_ls(lua_State *L) {
    const char *path = luaL_optstring(L, 1, ".");
    DIR dir;
    FILINFO fno;

    if (f_opendir(&dir, path) != FR_OK) {
        printf("cannot open: %s\n", path);
        return 0;
    }

    while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0') {
        if (fno.fattrib & AM_DIR) {
            printf("  [dir] %s\n", fno.fname);
            p8_console_printf(" [dir] %s\n", fno.fname);
        } else {
            printf("  %s (%u)\n", fno.fname, (unsigned)fno.fsize);
            p8_console_printf(" %s (%u)\n", fno.fname, (unsigned)fno.fsize);
        }
    }

    f_closedir(&dir);
    // Redraw console so output is visible
    p8_console_draw();
    gfx_flip();
    return 0;
}

static void setup_lua_sd(lua_State *L) {
    // Override dofile
    lua_pushcfunction(L, lua_custom_dofile);
    lua_setglobal(L, "dofile");

    // Override loadfile
    lua_pushcfunction(L, lua_custom_loadfile);
    lua_setglobal(L, "loadfile");

    // Set package.path for SD card
    lua_getglobal(L, "package");
    lua_pushstring(L, "/?.lua;/?/init.lua;/?.p8");
    lua_setfield(L, -2, "path");
    lua_pop(L, 1);

    // Add our SD searcher to package.searchers (insert at position 2)
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    int n = (int)luaL_len(L, -1);

    // Shift existing searchers up by one
    for (int i = n; i >= 2; i--) {
        lua_rawgeti(L, -1, i);
        lua_rawseti(L, -2, i + 1);
    }

    // Insert our searcher at position 2
    lua_pushcfunction(L, lua_sd_searcher);
    lua_rawseti(L, -2, 2);

    lua_pop(L, 2); // pop searchers and package
}

// Try to auto-run /main.lua or /main.p8, return true if it ran (with or without errors)
static bool try_autorun(lua_State *L) {
    FILINFO fno;

    // Check for .p8 or .p8.png cart first (uses full cart loader with game loop)
    if (f_stat("/main.p8", &fno) == FR_OK) {
        p8_cart_run(L, "/main.p8");
        return true;
    }
    if (f_stat("/main.p8.png", &fno) == FR_OK) {
        p8_cart_run(L, "/main.p8.png");
        return true;
    }

    // Plain .lua script (original behavior)
    if (f_stat("/main.lua", &fno) != FR_OK)
        return false;

    printf("Running /main.lua...\n");

    size_t sz;
    char *buf = load_and_preprocess("/main.lua", &sz);
    if (!buf) {
        printf("ERROR: could not read /main.lua\n");
        return true;
    }

    int status = luaL_loadbuffer(L, buf, sz, "@/main.lua");
    tlsf_free(psram_tlsf, buf);

    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) printf("ERROR: %s\n", err);
        lua_pop(L, 1);
        return true;
    }

    status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        if (err) printf("ERROR: %s\n", err);
        lua_pop(L, 1);
    }
    return true;
}

static constexpr int LINE_BUF_SIZE = 256;

int main() {
    // Overclock to 252 MHz (2x default). VCO=1260MHz (12*105), postdiv 5/1.
    // Requires 1.3V core voltage. Tested and stable.
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(10); // let voltage stabilize
    set_sys_clock_pll(1260000000, 5, 1); // VCO=1260MHz, postdiv1=5, postdiv2=1 → 252MHz

    // Enable 5V VBUS power to USB-A host port (GPIO 11 controls power switch)
    gpio_init(11);
    gpio_set_dir(11, GPIO_OUT);
    gpio_put(11, 1);

    // Configure PIO-USB host on port 1 (D+ = GPIO1, D- = GPIO2).
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = 1;
    pio_cfg.tx_ch = 9;
    tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);

    // tusb_init() called inside here inits device CDC (port 0) + PIO-USB host (port 1)
    stdio_init_all();

    // Start DVI display immediately so screen is live during boot
    gfx_init();
    dvi_init(gfx_get_dvi_buffer());
    p8_console_init();
    p8_console_print("wili8jam 0.10\n");
    p8_console_draw();
    gfx_flip();

    // Register keyboard callback for input state tracking
    obUSBHost.m_obHID.getKeyboard().setKeyCallback(input_key_callback);

    // Register modifier key polling so Ctrl/Shift/Alt state is synced each frame
    input_set_modifier_poll([]() -> uint8_t {
        return obUSBHost.m_obHID.getKeyboard().getModifiers();
    });

    // Register mouse polling function — always update when mounted
    // (getDeltas resets accumulators, so we must always pass them through)
    input_set_mouse_poll([]() {
        auto &mouse = obUSBHost.m_obHID.getMouse();
        if (mouse.isAnyMounted()) {
            int32_t dx, dy, wheel;
            mouse.getDeltas(&dx, &dy, &wheel);
            input_mouse_update(dx, dy, wheel, mouse.getButtons());
        }
    });

    // Register controller callback — player assigned by mount order
    obUSBHost.m_obHID.getController().setReportCallback(
        [](uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
            auto &ctrl = obUSBHost.m_obHID.getController();
            int player = ctrl.getPlayerForDevice(dev_addr, instance);
            if (player > 1) player = 1; // PICO-8 supports 2 players max
            input_gamepad_report(report, len, player);
        });

    // NOTE: Generic HID devices are NOT routed to input_gamepad_report.
    // Many keyboards expose a secondary HID interface (consumer control)
    // with protocol=NONE that gets classified as "generic". Feeding those
    // reports through the gamepad parser causes phantom directional input
    // (e.g., constant left) because zero-filled reports are misinterpreted
    // as axis values. Only properly-detected controllers (via isController)
    // should produce gamepad input.

    // Register XInput (Xbox) controller callback
    obUSBHost.m_obXInput.setReportCallback(
        [](uint8_t dev_addr, uint8_t instance, xinput_gamepad_t const *pad) {
            int player = obUSBHost.m_obXInput.getPlayerForDevice(dev_addr, instance);
            if (player > 1) player = 1;
            input_xinput_update(pad->wButtons, pad->sThumbLX, pad->sThumbLY, player);
        });

    printf("\n");
    printf("========================================\n");
    printf("  wili8jam — Lua 5.4.7 on Fruit Jam\n");
    printf("  RP2350B @ %u MHz | USB Serial REPL\n", (unsigned)(clock_get_hz(clk_sys) / 1000000));
    printf("========================================\n");

    // Init PSRAM
    psram_total_size = setup_psram();
    if (psram_total_size == 0) {
        p8_console_print("err: no psram!\n");
        p8_console_draw(); gfx_flip();
        printf("ERROR: PSRAM not detected!\n");
        while (true) tight_loop_contents();
    }
    printf("PSRAM: %u KB detected\n", (unsigned)(psram_total_size / 1024));
    p8_console_printf("psram: %u kb\n", (unsigned)(psram_total_size / 1024));
    p8_console_draw(); gfx_flip();

    // Init TLSF allocator on PSRAM
    psram_tlsf = tlsf_create_with_pool((void*)PSRAM_BASE, psram_total_size);
    if (!psram_tlsf) {
        printf("ERROR: Failed to init TLSF on PSRAM\n");
        while (true) tight_loop_contents();
    }
    printf("TLSF heap ready.\n");

    // Init PICO-8 preprocessor with PSRAM allocator
    p8_preprocess_init(psram_tlsf);

    // Init PICO-8 virtual memory (32KB in PSRAM)
    p8_init(psram_tlsf);

    // Init cart loader
    p8_cart_init(psram_tlsf);

    // Init editor
    p8_editor_init(psram_tlsf);

    // Init SD card
    printf("SD card: ");
    if (f_mount(&fatfs, "", 1) == FR_OK) {
        sd_mounted = true;
        const char *type_str = "unknown";
        switch (sd_get_type()) {
            case SD_TYPE_SDv1: type_str = "SDv1"; break;
            case SD_TYPE_SDv2: type_str = "SDv2"; break;
            case SD_TYPE_SDHC: type_str = "SDHC"; break;
        }
        printf("mounted (%s)\n", type_str);
        p8_console_printf("sd: %s\n", type_str);
    } else {
        printf("not found (REPL only)\n");
        p8_console_print("sd: not found\n");
    }
    p8_console_draw(); gfx_flip();

    printf("DVI: 640x480 output started\n");

    // Init audio (codec + I2S + DMA)
    if (audio_init()) {
        printf("Audio: I2S + DAC ready\n");
        p8_console_print("audio: ready\n");
    } else {
        printf("Audio: init failed\n");
        p8_console_print("audio: failed\n");
    }

    // Init PICO-8 SFX/music engine (wavetables + pitch table)
    p8_sfx_init();

    // Poll USB host to enumerate devices already plugged in at boot.
    // Xbox One controllers need multiple round-trips for power-on + init handshake,
    // and PIO-USB full-speed enumeration is slower than native USB.
    // Poll for up to 3 seconds, exit early once a device is detected.
    for (int i = 0; i < 300; i++) {
        tuh_task();
        sleep_ms(10);
        // Exit early once any HID or XInput device is mounted
        if (i > 50 && (obUSBHost.m_obHID.isKeyboardMounted() ||
                       obUSBHost.m_obHID.isControllerMounted() ||
                       obUSBHost.m_obXInput.isAnyMounted() ||
                       obUSBHost.m_obHID.getGenericCount() > 0)) {
            printf("[USB] Device detected after %d ms\n", i * 10);
            break;
        }
    }
    printf("[USB] Boot scan complete: kbd=%d ctrl=%d xinput=%d generic=%d\n",
        obUSBHost.m_obHID.getKeyboardCount(),
        obUSBHost.m_obHID.getControllerCount(),
        obUSBHost.m_obXInput.getMountedCount(),
        obUSBHost.m_obHID.getGenericCount());

    printf("Ready. Type Lua code, press Enter.\n");
    p8_console_print("ready.\n");
    p8_console_draw();
    gfx_flip();

    lua_State *L = lua_newstate(lua_psram_alloc, NULL);
    if (!L) {
        printf("ERROR: Failed to create Lua state\n");
        while (true) tight_loop_contents();
    }
    luaL_openlibs(L);

    // Register PICO-8 API globals (must be after openlibs since it overrides print, etc.)
    p8_register_api(L);

    // Register cart lifecycle functions (load, reset, run)
    p8_cart_register(L);

    // Register editor
    p8_editor_register(L);

    // Register console print (overrides p8_api's print for REPL mode)
    p8_console_register(L);

    // Register sys library
    luaL_requiref(L, "sys", luaopen_sys, 1);
    lua_pop(L, 1);

    // Register gfx library
    luaL_requiref(L, "gfx", luaopen_gfx, 1);
    lua_pop(L, 1);

    // Register input library
    luaL_requiref(L, "input", luaopen_input, 1);
    lua_pop(L, 1);

    // Register audio library
    luaL_requiref(L, "audio", luaopen_audio, 1);
    lua_pop(L, 1);

    // Register fs library and set up SD overrides (if mounted)
    if (sd_mounted) {
        luaL_requiref(L, "fs", luaopen_fs, 1);
        lua_pop(L, 1);
        setup_lua_sd(L);
        lua_register(L, "ls", lua_ls);
        lua_register(L, "mkdir", lua_mkdir);
        lua_register(L, "rm", lua_rm);
        lua_register(L, "cd", lua_cd);
        try_autorun(L);
    }
    lua_register(L, "help", lua_help);
    lua_register(L, "info", lua_info);
    lua_register(L, "reboot", lua_reboot);

    // Snapshot all built-in globals — anything registered after this point
    // or by carts will be cleaned up on cart switch
    p8_snapshot_globals(L);

    char line[LINE_BUF_SIZE];
    int pos = 0;
    bool skip_console_redraw = false; // set after drawing commands to preserve screen

    // Helper lambda-like: redraw console + input line on DVI
    auto repl_redraw = [&]() {
        if (!skip_console_redraw)
            p8_console_draw();
        // Draw input line at bottom
        int input_y = 122; // bottom of screen (122+5=127, fits in 128px)
        gfx_rectfill(0, 120, 127, 127, 0); // clear input area
        gfx_print_w(">", 0, input_y, 12, 4); // blue prompt
        if (pos > 0) {
            // Show last ~30 chars of input (4px per char)
            int show_start = pos > 30 ? pos - 30 : 0;
            char tmp[32];
            int n = pos - show_start;
            if (n > 31) n = 31;
            memcpy(tmp, line + show_start, n);
            tmp[n] = '\0';
            gfx_print_w(tmp, 4, input_y, 7, 4);
        }
        // Blinking cursor
        static int blink = 0;
        blink++;
        if (blink & 16) {
            int cx = 4 + ((pos > 30 ? 30 : pos) * 4);
            gfx_rectfill(cx, input_y, cx + 3, input_y + 5, 7);
        }
        gfx_flip();
    };

    while (true) {
        printf("> ");

        pos = 0;
        while (pos < LINE_BUF_SIZE - 1) {
            // Poll USB host while waiting for serial input
            tuh_task();

            // Redraw console on screen periodically
            repl_redraw();

            // Check for ESC key → toggle to editor
            input_update();
            if (input_key(0x29)) { // HID_KEY_ESCAPE
                input_flush();
                p8_editor_enter();
                // Wait for ESC release (debounce from editor toggle)
                while (input_key(0x29)) { tuh_task(); input_update(); sleep_ms(10); }
                skip_console_redraw = false;
                p8_console_draw();
                gfx_flip();
                printf("> ");
                continue;
            }

            // Read from serial OR USB keyboard
            int c = getchar_timeout_us(0);
            if (c == PICO_ERROR_TIMEOUT)
                c = input_getchar();
            if (c < 0) {
                sleep_ms(16);
                continue;
            }
            if (c == '\r' || c == '\n') {
                printf("\n");
                skip_console_redraw = false;
                break;
            }
            if (c == '\b' || c == 127) {
                if (pos > 0) {
                    pos--;
                    printf("\b \b");
                }
                continue;
            }
            line[pos++] = static_cast<char>(c);
            putchar(c);
        }
        line[pos] = '\0';

        // Skip empty lines
        if (pos == 0) continue;

        // Echo command to console
        p8_console_printf("> %s\n", line);

        // Handle '= expr' shorthand (evaluate and print expression)
        const char *code = line;
        char expr_buf[LINE_BUF_SIZE + 16];
        if (line[0] == '=') {
            snprintf(expr_buf, sizeof(expr_buf), "print(%s)", line + 1);
            code = expr_buf;
        }

        // Handle PICO-8 bare commands (e.g., 'cls' → 'cls()', 'ls' → 'ls()')
        {
            // Trim leading whitespace for comparison
            const char *trimmed = line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            // List of PICO-8 commands that can be invoked without ()
            static const char *bare_cmds[] = {
                "cls", "ls", "cd", "reboot", "reset", "run", "stop",
                "resume", "flip", "save", "load", "folder",
                "info", "help", "splore", "install_demos", "edit",
                NULL
            };
            for (int i = 0; bare_cmds[i]; i++) {
                if (strcmp(trimmed, bare_cmds[i]) == 0) {
                    snprintf(expr_buf, sizeof(expr_buf), "%s()", trimmed);
                    code = expr_buf;
                    break;
                }
            }
        }

        // Handle PICO-8 'edit name' shorthand (without quotes)
        if (strncmp(line, "edit ", 5) == 0 && line[5] != '(' && line[5] != '"') {
            char *name = line + 5;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "edit(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }

        // Handle PICO-8 'save name' shorthand (without quotes)
        if (strncmp(line, "save ", 5) == 0 && line[5] != '(' && line[5] != '"') {
            char *name = line + 5;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "save(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }

        // Handle PICO-8 'load name' shorthand (without quotes)
        if (strncmp(line, "load ", 5) == 0 && line[5] != '(' && line[5] != '"') {
            // Trim trailing whitespace
            char *name = line + 5;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "load(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }

        // Handle 'cd path' shorthand
        if (strncmp(line, "cd ", 3) == 0 && line[3] != '(' && line[3] != '"') {
            char *name = line + 3;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "cd(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }
        // Handle 'help cmd' shorthand
        if (strncmp(line, "help ", 5) == 0 && line[5] != '(' && line[5] != '"') {
            char *name = line + 5;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "help(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }
        // Handle 'mkdir name' and 'rm name' shorthands
        if (strncmp(line, "mkdir ", 6) == 0 && line[6] != '(' && line[6] != '"') {
            char *name = line + 6;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "mkdir(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }
        if (strncmp(line, "rm ", 3) == 0 && line[3] != '(' && line[3] != '"') {
            char *name = line + 3;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "rm(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }
        // Handle 'ls path' shorthand
        if (strncmp(line, "ls ", 3) == 0 && line[3] != '(' && line[3] != '"') {
            char *name = line + 3;
            while (*name == ' ') name++;
            int nlen = strlen(name);
            while (nlen > 0 && (name[nlen-1] == ' ' || name[nlen-1] == '\t')) nlen--;
            if (nlen > 0) {
                snprintf(expr_buf, sizeof(expr_buf), "ls(\"%.*s\")", nlen, name);
                code = expr_buf;
            }
        }

        // Run PICO-8 preprocessor on REPL input
        size_t pp_len;
        char *pp_code = p8_preprocess(code, strlen(code), &pp_len);
        if (pp_code) code = pp_code;

        int status = luaL_dostring(L, code);
        if (pp_code) tlsf_free(psram_tlsf, pp_code);
        if (status != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            if (err) {
                printf("ERROR: %s\n", err);
                p8_console_printf("error: %s\n", err);
            }
            lua_pop(L, 1);
        }

        // Flip to show any drawing commands executed by the user.
        // Skip console redraw in input loop so drawings remain visible.
        // Console reappears when user presses Enter for next command.
        skip_console_redraw = true;
        gfx_flip();
    }

    lua_close(L);
    return 0;
}
