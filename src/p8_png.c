/*
 * p8_png.c — .p8.png cartridge loader
 *
 * Parses PNG, extracts steganographic cart data from pixel LSBs,
 * loads asset sections into PICO-8 virtual memory, and decompresses
 * the Lua code section (old :c: format or new PXA format).
 *
 * Contains a minimal DEFLATE decompressor for PNG IDAT chunks.
 */

#include "p8_png.h"
#include "p8_api.h"
#include "tlsf/tlsf.h"
#include <string.h>
#include <stdio.h>

static tlsf_t png_tlsf;

void p8_png_init(tlsf_t tlsf) {
    png_tlsf = tlsf;
}

// ============================================================
// Bit reader (LSB-first, used by both DEFLATE and PXA)
// ============================================================

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t byte_pos;
    int bit_pos;     // 0-7, LSB first
} bitreader_t;

static void br_init(bitreader_t *br, const uint8_t *data, size_t len) {
    br->data = data;
    br->len = len;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

static int br_bit(bitreader_t *br) {
    if (br->byte_pos >= br->len) return 0;
    int bit = (br->data[br->byte_pos] >> br->bit_pos) & 1;
    br->bit_pos++;
    if (br->bit_pos >= 8) { br->bit_pos = 0; br->byte_pos++; }
    return bit;
}

static uint32_t br_bits(bitreader_t *br, int n) {
    uint32_t val = 0;
    for (int i = 0; i < n; i++)
        val |= (uint32_t)br_bit(br) << i;
    return val;
}

// Align to next byte boundary
static void br_align(bitreader_t *br) {
    if (br->bit_pos > 0) { br->bit_pos = 0; br->byte_pos++; }
}

// ============================================================
// DEFLATE: Huffman table
// ============================================================

#define HUFF_MAX_BITS 15
#define HUFF_MAX_SYMS 320  // max(288 lit/len, 32 dist, 19 codelen)

typedef struct {
    uint16_t counts[HUFF_MAX_BITS + 1]; // number of codes of each length
    uint16_t symbols[HUFF_MAX_SYMS];    // symbols sorted by code
} huff_t;

static void huff_build(huff_t *h, const uint8_t *lengths, int num_syms) {
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < num_syms; i++)
        if (lengths[i]) h->counts[lengths[i]]++;

    // Compute offsets
    uint16_t offsets[HUFF_MAX_BITS + 1];
    offsets[0] = 0;
    for (int i = 1; i <= HUFF_MAX_BITS; i++)
        offsets[i] = offsets[i - 1] + h->counts[i - 1];

    for (int i = 0; i < num_syms; i++)
        if (lengths[i])
            h->symbols[offsets[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(huff_t *h, bitreader_t *br) {
    uint32_t code = 0;
    uint32_t first = 0;
    int index = 0;
    for (int len = 1; len <= HUFF_MAX_BITS; len++) {
        code |= (uint32_t)br_bit(br);
        int count = h->counts[len];
        if ((int)(code - first) < count)
            return h->symbols[index + (int)(code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1; // invalid
}

// ============================================================
// DEFLATE decompressor
// ============================================================

// Length base values and extra bits (codes 257-285)
static const uint16_t len_base[] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t len_extra[] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};

// Distance base values and extra bits (codes 0-29)
static const uint16_t dist_base[] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t dist_extra[] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

// Code length order for dynamic Huffman
static const uint8_t codelen_order[] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

// Build fixed Huffman tables
static void build_fixed_tables(huff_t *lit, huff_t *dist) {
    uint8_t lengths[288];
    int i;
    for (i = 0; i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    huff_build(lit, lengths, 288);

    for (i = 0; i < 32; i++) lengths[i] = 5;
    huff_build(dist, lengths, 32);
}

// Inflate compressed data. Returns allocated output buffer and sets *out_len.
// Returns NULL on failure.
static uint8_t *inflate(const uint8_t *src, size_t src_len, size_t *out_len, size_t max_out) {
    bitreader_t br;
    br_init(&br, src, src_len);

    size_t cap = max_out ? max_out : 65536;
    uint8_t *out = (uint8_t *)tlsf_malloc(png_tlsf, cap);
    if (!out) return NULL;
    size_t pos = 0;

    int bfinal;
    do {
        bfinal = br_bit(&br);
        int btype = (int)br_bits(&br, 2);

        if (btype == 0) {
            // Stored block
            br_align(&br);
            if (br.byte_pos + 4 > br.len) goto fail;
            uint16_t len = br.data[br.byte_pos] | (br.data[br.byte_pos + 1] << 8);
            br.byte_pos += 4; // skip len and ~len
            if (br.byte_pos + len > br.len) goto fail;
            if (pos + len > cap) goto fail;
            memcpy(out + pos, br.data + br.byte_pos, len);
            pos += len;
            br.byte_pos += len;
        } else if (btype == 1 || btype == 2) {
            huff_t lit_h, dist_h;

            if (btype == 1) {
                build_fixed_tables(&lit_h, &dist_h);
            } else {
                // Dynamic Huffman tables
                int hlit = (int)br_bits(&br, 5) + 257;
                int hdist = (int)br_bits(&br, 5) + 1;
                int hclen = (int)br_bits(&br, 4) + 4;

                uint8_t codelen_lengths[19];
                memset(codelen_lengths, 0, sizeof(codelen_lengths));
                for (int ci = 0; ci < hclen; ci++)
                    codelen_lengths[codelen_order[ci]] = (uint8_t)br_bits(&br, 3);

                huff_t cl_h;
                huff_build(&cl_h, codelen_lengths, 19);

                uint8_t all_lengths[320];
                memset(all_lengths, 0, sizeof(all_lengths));
                int total = hlit + hdist;
                int ai = 0;
                while (ai < total) {
                    int sym = huff_decode(&cl_h, &br);
                    if (sym < 0) goto fail;
                    if (sym < 16) {
                        all_lengths[ai++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        int rep = (int)br_bits(&br, 2) + 3;
                        uint8_t prev = ai > 0 ? all_lengths[ai - 1] : 0;
                        for (int r = 0; r < rep && ai < total; r++)
                            all_lengths[ai++] = prev;
                    } else if (sym == 17) {
                        int rep = (int)br_bits(&br, 3) + 3;
                        for (int r = 0; r < rep && ai < total; r++)
                            all_lengths[ai++] = 0;
                    } else if (sym == 18) {
                        int rep = (int)br_bits(&br, 7) + 11;
                        for (int r = 0; r < rep && ai < total; r++)
                            all_lengths[ai++] = 0;
                    }
                }

                huff_build(&lit_h, all_lengths, hlit);
                huff_build(&dist_h, all_lengths + hlit, hdist);
            }

            // Decode lit/len + dist pairs
            while (1) {
                int sym = huff_decode(&lit_h, &br);
                if (sym < 0) goto fail;
                if (sym == 256) break; // end of block
                if (sym < 256) {
                    if (pos >= cap) goto fail;
                    out[pos++] = (uint8_t)sym;
                } else {
                    // Length code
                    int li = sym - 257;
                    if (li >= 29) goto fail;
                    int length = len_base[li] + (int)br_bits(&br, len_extra[li]);

                    // Distance code
                    int di = huff_decode(&dist_h, &br);
                    if (di < 0 || di >= 30) goto fail;
                    int distance = dist_base[di] + (int)br_bits(&br, dist_extra[di]);

                    if ((size_t)distance > pos) goto fail;
                    if (pos + length > cap) goto fail;

                    for (int ci = 0; ci < length; ci++)
                        out[pos + ci] = out[pos - distance + ci];
                    pos += length;
                }
            }
        } else {
            goto fail; // reserved block type
        }
    } while (!bfinal);

    *out_len = pos;
    return out;

fail:
    tlsf_free(png_tlsf, out);
    return NULL;
}

// ============================================================
// PNG parser
// ============================================================

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

// Parse PNG and extract raw RGBA pixels (160x205).
// Returns allocated RGBA buffer (160*205*4 bytes) or NULL.
static uint8_t *png_decode(const uint8_t *data, size_t data_len,
                            int *out_w, int *out_h) {
    // Check PNG signature
    static const uint8_t png_sig[] = {137,80,78,71,13,10,26,10};
    if (data_len < 8 || memcmp(data, png_sig, 8) != 0) return NULL;

    size_t pos = 8;
    int width = 0, height = 0, bit_depth = 0, color_type = 0;
    bool got_ihdr = false;

    // First pass: find IHDR and compute total IDAT size
    size_t idat_total = 0;
    size_t scan_pos = pos;
    while (scan_pos + 12 <= data_len) {
        uint32_t chunk_len = read_be32(data + scan_pos);
        const uint8_t *ctype = data + scan_pos + 4;
        const uint8_t *cdata = data + scan_pos + 8;

        if (memcmp(ctype, "IHDR", 4) == 0 && chunk_len >= 13) {
            width = (int)read_be32(cdata);
            height = (int)read_be32(cdata + 4);
            bit_depth = cdata[8];
            color_type = cdata[9];
            got_ihdr = true;
        }
        if (memcmp(ctype, "IDAT", 4) == 0) idat_total += chunk_len;
        if (memcmp(ctype, "IEND", 4) == 0) break;
        scan_pos += 12 + chunk_len;
    }

    if (!got_ihdr || width <= 0 || height <= 0) return NULL;
    if (bit_depth != 8 || (color_type != 6 && color_type != 2)) return NULL;

    int bpp = (color_type == 6) ? 4 : 3; // bytes per pixel (RGBA or RGB)

    // Concatenate all IDAT chunk data
    uint8_t *idat_buf = (uint8_t *)tlsf_malloc(png_tlsf, idat_total);
    if (!idat_buf) return NULL;

    size_t idat_pos = 0;
    scan_pos = 8;
    while (scan_pos + 12 <= data_len) {
        uint32_t chunk_len = read_be32(data + scan_pos);
        const uint8_t *ctype = data + scan_pos + 4;
        const uint8_t *cdata = data + scan_pos + 8;

        if (memcmp(ctype, "IDAT", 4) == 0) {
            memcpy(idat_buf + idat_pos, cdata, chunk_len);
            idat_pos += chunk_len;
        }
        if (memcmp(ctype, "IEND", 4) == 0) break;
        scan_pos += 12 + chunk_len;
    }

    // Skip zlib header (2 bytes: CMF + FLG)
    if (idat_total < 2) { tlsf_free(png_tlsf, idat_buf); return NULL; }

    // Decompress
    size_t raw_len = (size_t)(1 + width * bpp) * height; // filter byte + pixels per row
    size_t inflated_len;
    uint8_t *raw = inflate(idat_buf + 2, idat_total - 2, &inflated_len, raw_len + 1024);
    tlsf_free(png_tlsf, idat_buf);

    if (!raw || inflated_len < raw_len) {
        if (raw) tlsf_free(png_tlsf, raw);
        return NULL;
    }

    // Allocate output RGBA buffer
    size_t out_size = (size_t)(width * height * 4);
    uint8_t *pixels = (uint8_t *)tlsf_malloc(png_tlsf, out_size);
    if (!pixels) { tlsf_free(png_tlsf, raw); return NULL; }

    // Unfilter rows
    int stride = width * bpp;
    for (int y = 0; y < height; y++) {
        uint8_t *row = raw + y * (1 + stride);
        uint8_t filter = row[0];
        uint8_t *cur = row + 1;
        uint8_t *prev = (y > 0) ? raw + (y - 1) * (1 + stride) + 1 : NULL;

        for (int x = 0; x < stride; x++) {
            uint8_t a = (x >= bpp) ? cur[x - bpp] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t c = (prev && x >= bpp) ? prev[x - bpp] : 0;

            switch (filter) {
                case 0: break; // None
                case 1: cur[x] += a; break; // Sub
                case 2: cur[x] += b; break; // Up
                case 3: cur[x] += (uint8_t)((a + b) / 2); break; // Average
                case 4: { // Paeth
                    int p = (int)a + (int)b - (int)c;
                    int pa = p - (int)a; if (pa < 0) pa = -pa;
                    int pb = p - (int)b; if (pb < 0) pb = -pb;
                    int pc = p - (int)c; if (pc < 0) pc = -pc;
                    cur[x] += (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
                    break;
                }
            }
        }

        // Copy to RGBA output
        for (int x = 0; x < width; x++) {
            uint8_t *dst = pixels + (y * width + x) * 4;
            if (bpp == 4) {
                dst[0] = cur[x * 4 + 0]; // R
                dst[1] = cur[x * 4 + 1]; // G
                dst[2] = cur[x * 4 + 2]; // B
                dst[3] = cur[x * 4 + 3]; // A
            } else {
                dst[0] = cur[x * 3 + 0]; // R
                dst[1] = cur[x * 3 + 1]; // G
                dst[2] = cur[x * 3 + 2]; // B
                dst[3] = 0xFF;            // A (opaque)
            }
        }
    }

    tlsf_free(png_tlsf, raw);
    *out_w = width;
    *out_h = height;
    return pixels;
}

// ============================================================
// Steganography extraction
// ============================================================

// Extract cart data from RGBA pixels. Each pixel contributes 1 byte
// using the 2 LSBs of each channel: (A&3)<<6 | (R&3)<<4 | (G&3)<<2 | (B&3)
static void extract_cart_data(const uint8_t *pixels, int w, int h,
                               uint8_t *cart, size_t cart_size) {
    size_t pi = 0; // pixel index
    for (size_t i = 0; i < cart_size && pi < (size_t)(w * h); i++, pi++) {
        const uint8_t *px = pixels + pi * 4;
        uint8_t r = px[0], g = px[1], b = px[2], a = px[3];
        cart[i] = (uint8_t)(((a & 3) << 6) | ((r & 3) << 4) |
                             ((g & 3) << 2) | (b & 3));
    }
}

// ============================================================
// PICO-8 code decompression — old format (:c:)
// ============================================================

static const char old_lookup[] =
    "\n 0123456789abcdefghijklmnopqrstuvwxyz!#%(){}[]<>+=/*:;.,~_";

static char *decompress_old(const uint8_t *src, size_t src_len,
                             size_t decomp_len, size_t *out_len) {
    char *out = (char *)tlsf_malloc(png_tlsf, decomp_len + 1);
    if (!out) return NULL;

    size_t si = 0, oi = 0;
    while (si < src_len && oi < decomp_len) {
        uint8_t b = src[si];
        if (b == 0x00) {
            if (si + 1 >= src_len) break;
            out[oi++] = (char)src[si + 1];
            si += 2;
        } else if (b <= 0x3b) {
            if ((int)(b - 1) < (int)sizeof(old_lookup) - 1)
                out[oi++] = old_lookup[b - 1];
            si++;
        } else {
            // Copy reference
            if (si + 1 >= src_len) break;
            uint8_t next = src[si + 1];
            int offset = (b - 0x3c) * 16 + (next & 0x0f);
            int length = (next >> 4) + 2;
            if (offset > (int)oi) break; // invalid reference
            for (int ci = 0; ci < length && oi < decomp_len; ci++)
                out[oi + ci] = out[oi - offset + ci];
            oi += length;
            si += 2;
        }
    }

    out[oi] = '\0';
    *out_len = oi;
    return out;
}

// ============================================================
// PICO-8 code decompression — new PXA format
// ============================================================

static char *decompress_pxa(const uint8_t *src, size_t src_len,
                             size_t decomp_len, size_t *out_len) {
    char *out = (char *)tlsf_malloc(png_tlsf, decomp_len + 1);
    if (!out) return NULL;

    // Initialize move-to-front table
    uint8_t mtf[256];
    for (int i = 0; i < 256; i++) mtf[i] = (uint8_t)i;

    bitreader_t br;
    br_init(&br, src, src_len);

    size_t oi = 0;
    while (oi < decomp_len) {
        int header = br_bit(&br);

        if (header == 1) {
            // MTF character
            int unary = 0;
            while (br_bit(&br) == 1) unary++;

            int unary_mask = (1 << unary) - 1;
            int index = (int)br_bits(&br, 4 + unary) + (unary_mask << 4);

            if (index >= 256) break; // invalid

            uint8_t ch = mtf[index];
            // Move to front
            for (int i = index; i > 0; i--) mtf[i] = mtf[i - 1];
            mtf[0] = ch;

            out[oi++] = (char)ch;
        } else {
            // Copy reference
            int offset_bits = br_bit(&br);
            int length_bits = br_bit(&br);
            int offset = (int)br_bits(&br, 3 + offset_bits);
            int length = (int)br_bits(&br, 4 + length_bits);

            if (offset == 0 || offset > (int)oi) break; // invalid

            for (int ci = 0; ci < length && oi < decomp_len; ci++)
                out[oi + ci] = out[oi - offset + ci];
            oi += length;
        }
    }

    out[oi] = '\0';
    *out_len = oi;
    return out;
}

// ============================================================
// Main .p8.png loader
// ============================================================

char *p8_png_load(const uint8_t *data, size_t data_len, size_t *lua_len) {
    if (!png_tlsf) return NULL;

    // Decode PNG to RGBA pixels
    int w, h;
    uint8_t *pixels = png_decode(data, data_len, &w, &h);
    if (!pixels) return NULL;

    if (w != 160 || h != 205) {
        tlsf_free(png_tlsf, pixels);
        return NULL;
    }

    // Extract cart data from pixel LSBs
    // Total extractable: 160*205 = 32800 bytes, but cart is 0x8000 (32768) + header
    uint8_t *cart = (uint8_t *)tlsf_malloc(png_tlsf, 0x8020);
    if (!cart) { tlsf_free(png_tlsf, pixels); return NULL; }
    memset(cart, 0, 0x8020);
    extract_cart_data(pixels, w, h, cart, 0x8020);
    tlsf_free(png_tlsf, pixels);

    // Copy asset data (0x0000-0x42FF) into PICO-8 virtual memory
    uint8_t *mem = p8_get_memory();
    if (mem) {
        memset(mem, 0, 0x8000);
        // Sprite sheet (0x0000-0x1FFF)
        memcpy(mem, cart, 0x2000);
        // Map (0x2000-0x2FFF)
        memcpy(mem + 0x2000, cart + 0x2000, 0x1000);
        // Sprite flags (0x3000-0x30FF)
        memcpy(mem + 0x3000, cart + 0x3000, 0x100);
        // Music (0x3100-0x31FF)
        memcpy(mem + 0x3100, cart + 0x3100, 0x100);
        // SFX (0x3200-0x42FF)
        memcpy(mem + 0x3200, cart + 0x3200, 0x1100);
    }

    // Decompress code section (0x4300+)
    const uint8_t *code_section = cart + 0x4300;
    size_t code_section_len = 0x8000 - 0x4300; // max available

    char *lua_code = NULL;
    *lua_len = 0;

    // Detect format
    if (code_section[0] == 0x00 && code_section[1] == 0x70 &&
        code_section[2] == 0x78 && code_section[3] == 0x61) {
        // New PXA format
        size_t decomp_len = ((size_t)code_section[4] << 8) | code_section[5];
        lua_code = decompress_pxa(code_section + 8, code_section_len - 8,
                                   decomp_len, lua_len);
    } else if (code_section[0] == 0x3a && code_section[1] == 0x63 &&
               code_section[2] == 0x3a && code_section[3] == 0x00) {
        // Old :c: format
        size_t decomp_len = ((size_t)code_section[4] << 8) | code_section[5];
        lua_code = decompress_old(code_section + 8, code_section_len - 8,
                                   decomp_len, lua_len);
    } else {
        // Plaintext — find null terminator
        size_t plen = 0;
        while (plen < code_section_len && code_section[plen] != 0) plen++;
        lua_code = (char *)tlsf_malloc(png_tlsf, plen + 1);
        if (lua_code) {
            memcpy(lua_code, code_section, plen);
            lua_code[plen] = '\0';
            *lua_len = plen;
        }
    }

    tlsf_free(png_tlsf, cart);

    return lua_code;
}
