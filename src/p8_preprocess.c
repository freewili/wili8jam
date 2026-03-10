/*
 * p8_preprocess.c — PICO-8 Lua dialect → standard Lua 5.4 preprocessor
 *
 * Single-pass, line-oriented transform. Allocates output via TLSF on PSRAM.
 */

#include "p8_preprocess.h"
#include "tlsf/tlsf.h"
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>

static tlsf_t s_tlsf;

void p8_preprocess_init(void *tlsf_handle) {
    s_tlsf = (tlsf_t)tlsf_handle;
}

/* --- Dynamic buffer --- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} buf_t;

static bool buf_init(buf_t *b, size_t initial) {
    b->data = (char *)tlsf_malloc(s_tlsf, initial);
    if (!b->data) return false;
    b->len = 0;
    b->cap = initial;
    return true;
}

static bool buf_grow(buf_t *b, size_t need) {
    if (b->len + need <= b->cap) return true;
    size_t newcap = b->cap * 2;
    if (newcap < b->len + need) newcap = b->len + need;
    char *nd = (char *)tlsf_realloc(s_tlsf, b->data, newcap);
    if (!nd) return false;
    b->data = nd;
    b->cap = newcap;
    return true;
}

static bool buf_append(buf_t *b, const char *s, size_t n) {
    if (!buf_grow(b, n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    return true;
}

static bool buf_putc(buf_t *b, char c) {
    return buf_append(b, &c, 1);
}

static bool buf_puts(buf_t *b, const char *s) {
    return buf_append(b, s, strlen(s));
}

/* --- Forward declarations for helpers used in post-pass --- */
static size_t skip_ws(const char *s, size_t i, size_t len);
static bool match_keyword(const char *s, size_t pos, size_t len, const char *kw);

/* --- String literal / long-string detection helpers --- */

/* Check if pos starts a long string/comment opening bracket: [=*[ */
static int long_bracket_level(const char *s, size_t pos, size_t len) {
    if (pos >= len || s[pos] != '[') return -1;
    size_t i = pos + 1;
    int level = 0;
    while (i < len && s[i] == '=') { level++; i++; }
    if (i < len && s[i] == '[') return level;
    return -1;
}

/* Skip past the closing long bracket ]=*] of given level. Returns index after it, or len if not found. */
static size_t skip_long_bracket(const char *s, size_t pos, size_t len, int level) {
    while (pos < len) {
        if (s[pos] == ']') {
            size_t j = pos + 1;
            int cnt = 0;
            while (j < len && s[j] == '=') { cnt++; j++; }
            if (cnt == level && j < len && s[j] == ']')
                return j + 1;
        }
        pos++;
    }
    return len;
}

/* Skip a quoted string starting at s[pos] (pos points to the opening quote).
   Returns index after closing quote, or len if unterminated. */
static size_t skip_string(const char *s, size_t pos, size_t len) {
    char q = s[pos];
    pos++;
    while (pos < len) {
        if (s[pos] == '\\') { pos += 2; continue; }
        if (s[pos] == q) return pos + 1;
        if (s[pos] == '\n') return pos; /* unterminated */
        pos++;
    }
    return len;
}

/* --- Line extraction helper --- */

/* Get the next line from src starting at *pos. Sets *line_start, *line_len.
   Advances *pos past the newline. Handles \r\n, \n, \r. */
static bool next_line(const char *src, size_t src_len, size_t *pos,
                      const char **line_start, size_t *line_len) {
    if (*pos >= src_len) return false;
    *line_start = src + *pos;
    size_t start = *pos;
    while (*pos < src_len && src[*pos] != '\n' && src[*pos] != '\r')
        (*pos)++;
    *line_len = *pos - start;
    /* consume newline */
    if (*pos < src_len) {
        if (src[*pos] == '\r') {
            (*pos)++;
            if (*pos < src_len && src[*pos] == '\n') (*pos)++;
        } else {
            (*pos)++;
        }
    }
    return true;
}

/* --- Identifier helpers --- */

static bool is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* --- Per-line transforms --- */

/*
 * Process a single line (without trailing newline) and append the
 * transformed result to buf. We track whether we're inside a long
 * string/comment that spans multiple lines via *in_long and *long_level.
 */
static bool process_line(buf_t *out, const char *line, size_t len,
                         bool *in_long, int *long_level);

/* Check if line (trimmed) starts with backslash for line continuation.
   PICO-8: backslash at end of line = continuation. We join lines. */

/* --- Main entry point --- */

char *p8_preprocess(const char *src, size_t src_len, size_t *out_len) {
    if (!s_tlsf) return NULL;

    /* Estimate output size: usually similar to input, with some expansion */
    buf_t out;
    if (!buf_init(&out, src_len + src_len / 4 + 256))
        return NULL;

    bool in_long = false;
    int long_level = 0;

    size_t pos = 0;
    const char *line;
    size_t line_len;

    /* Line continuation buffer */
    buf_t joined;
    if (!buf_init(&joined, 512)) {
        tlsf_free(s_tlsf, out.data);
        return NULL;
    }

    while (next_line(src, src_len, &pos, &line, &line_len)) {
        /* Handle backslash line continuation */
        if (!in_long && line_len > 0 && line[line_len - 1] == '\\') {
            buf_append(&joined, line, line_len - 1);
            buf_putc(&joined, ' ');
            continue;
        }

        const char *proc_line = line;
        size_t proc_len = line_len;

        if (joined.len > 0) {
            buf_append(&joined, line, line_len);
            proc_line = joined.data;
            proc_len = joined.len;
        }

        if (!process_line(&out, proc_line, proc_len, &in_long, &long_level)) {
            tlsf_free(s_tlsf, out.data);
            tlsf_free(s_tlsf, joined.data);
            return NULL;
        }
        buf_putc(&out, '\n');

        joined.len = 0;
    }

    /* Handle trailing continuation without final newline */
    if (joined.len > 0) {
        if (!process_line(&out, joined.data, joined.len, &in_long, &long_level)) {
            tlsf_free(s_tlsf, out.data);
            tlsf_free(s_tlsf, joined.data);
            return NULL;
        }
        buf_putc(&out, '\n');
    }

    tlsf_free(s_tlsf, joined.data);

    /* --- Post-pass: fix multi-line "if (...) do" → "if ... then" --- */
    /* PICO-8 allows if conditions to span multiple lines, with "do" on a
       subsequent line meaning "then". The line-by-line pass can't catch this.
       Scan the output for unresolved "if" followed by "do" and fix them. */
    {
        size_t i = 0;
        while (i < out.len) {
            /* Skip strings and comments */
            if (out.data[i] == '"' || out.data[i] == '\'') {
                i = skip_string(out.data, i, out.len);
                continue;
            }
            if (out.data[i] == '-' && i + 1 < out.len && out.data[i+1] == '-') {
                /* Skip to end of line */
                while (i < out.len && out.data[i] != '\n') i++;
                continue;
            }
            if (out.data[i] == '[') {
                int lvl = long_bracket_level(out.data, i, out.len);
                if (lvl >= 0) {
                    i = skip_long_bracket(out.data, i + lvl + 2, out.len, lvl);
                    continue;
                }
            }

            /* Look for "if" keyword */
            if (match_keyword(out.data, i, out.len, "if")) {
                size_t if_pos = i;
                i += 2;
                /* Skip to opening paren */
                size_t p = skip_ws(out.data, i, out.len);
                /* Skip newlines too */
                while (p < out.len && (out.data[p] == ' ' || out.data[p] == '\t' ||
                       out.data[p] == '\n' || out.data[p] == '\r')) p++;
                if (p < out.len && out.data[p] == '(') {
                    /* Find matching close paren (across lines) */
                    int depth = 1;
                    size_t q = p + 1;
                    while (q < out.len && depth > 0) {
                        char c = out.data[q];
                        if (c == '(') depth++;
                        else if (c == ')') { depth--; if (depth == 0) break; }
                        else if (c == '"' || c == '\'') { q = skip_string(out.data, q, out.len); continue; }
                        else if (c == '[') {
                            int lvl = long_bracket_level(out.data, q, out.len);
                            if (lvl >= 0) { q = skip_long_bracket(out.data, q + lvl + 2, out.len, lvl); continue; }
                        }
                        q++;
                    }
                    if (depth == 0) {
                        /* q points to ')'. Skip whitespace/newlines after it. */
                        size_t after = q + 1;
                        while (after < out.len && (out.data[after] == ' ' || out.data[after] == '\t' ||
                               out.data[after] == '\n' || out.data[after] == '\r')) after++;
                        /* Check if followed by "do" (not "then") */
                        if (match_keyword(out.data, after, out.len, "do")) {
                            /* Replace "do" with "then" — need to expand by 2 chars */
                            size_t do_pos = after;
                            if (buf_grow(&out, 2)) {
                                memmove(out.data + do_pos + 4, out.data + do_pos + 2, out.len - do_pos - 2);
                                memcpy(out.data + do_pos, "then", 4);
                                out.len += 2;
                            }
                            i = do_pos + 4;
                            continue;
                        }
                    }
                }
                i = if_pos + 2;
                continue;
            }
            i++;
        }
    }

    /* Null-terminate */
    if (!buf_putc(&out, '\0')) {
        tlsf_free(s_tlsf, out.data);
        return NULL;
    }

    if (out_len) *out_len = out.len - 1; /* exclude null terminator */
    return out.data;
}

/* Forward declaration for transform_line_content (called by process_line and try_short_form) */
static bool transform_line_content(buf_t *out, const char *line, size_t len,
                                   bool *in_long, int *long_level);

/* --- Process one line, handling long strings/comments spanning lines --- */

static bool process_line(buf_t *out, const char *line, size_t len,
                         bool *in_long, int *long_level) {
    if (*in_long) {
        /* We're inside a multi-line long string or long comment. Copy until close. */
        size_t i = 0;
        while (i < len) {
            if (line[i] == ']') {
                size_t j = i + 1;
                int cnt = 0;
                while (j < len && line[j] == '=') { cnt++; j++; }
                if (cnt == *long_level && j < len && line[j] == ']') {
                    /* Found closing bracket — output everything up to and including it */
                    buf_append(out, line, j + 1);
                    /* Process remainder of line normally */
                    *in_long = false;
                    if (j + 1 < len)
                        return transform_line_content(out, line + j + 1, len - j - 1,
                                                      in_long, long_level);
                    return true;
                }
            }
            i++;
        }
        /* Entire line is inside long string */
        buf_append(out, line, len);
        return true;
    }

    return transform_line_content(out, line, len, in_long, long_level);
}

/* --- Check for short-form if/while --- */

/* Skip whitespace, return new index */
static size_t skip_ws(const char *s, size_t i, size_t len) {
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Check if string at pos matches keyword (not followed by ident char) */
static bool match_keyword(const char *s, size_t pos, size_t len, const char *kw) {
    size_t klen = strlen(kw);
    if (pos + klen > len) return false;
    if (memcmp(s + pos, kw, klen) != 0) return false;
    if (pos + klen < len && is_ident_char(s[pos + klen])) return false;
    return true;
}

/* Find the matching close paren for an open paren at s[pos].
   Respects strings and nested parens. Returns index of ')' or len if not found. */
static size_t find_close_paren(const char *s, size_t pos, size_t len) {
    int depth = 1;
    pos++; /* skip opening '(' */
    while (pos < len && depth > 0) {
        char c = s[pos];
        if (c == '(' ) { depth++; pos++; }
        else if (c == ')') { depth--; if (depth == 0) return pos; pos++; }
        else if (c == '"' || c == '\'') { pos = skip_string(s, pos, len); }
        else if (c == '[') {
            int lvl = long_bracket_level(s, pos, len);
            if (lvl >= 0) pos = skip_long_bracket(s, pos + lvl + 2, len, lvl);
            else pos++;
        }
        else pos++;
    }
    return len;
}

/* Try to detect short-form: `if (cond) stmt` or `while (cond) stmt`.
   Returns true if it was a short-form and was emitted. */
static bool try_short_form(buf_t *out, const char *line, size_t len,
                           const char *keyword, const char *block_word, const char *block_end,
                           bool *in_long, int *long_level) {
    size_t i = skip_ws(line, 0, len);
    if (!match_keyword(line, i, len, keyword)) return false;

    size_t after_kw = i + strlen(keyword);
    size_t paren_start = skip_ws(line, after_kw, len);
    if (paren_start >= len || line[paren_start] != '(') return false;

    size_t paren_end = find_close_paren(line, paren_start, len);
    if (paren_end >= len) return false;

    /* Check what follows the close paren */
    size_t after_paren = skip_ws(line, paren_end + 1, len);

    /* If followed by 'then' (for if) or 'do' (for while), it's standard Lua.
       BUT: PICO-8 uses 'do' after 'if' as a block form meaning 'then'.
       Handle that: if keyword is "if" and followed by "do", rewrite "do" → "then"
       and do NOT append "end" (the source already has its own end). */
    if (match_keyword(line, after_paren, len, block_word))
        return false;

    /* PICO-8: "if (cond) do" → rewrite to "if cond then" (keep existing end) */
    if (strcmp(keyword, "if") == 0 && match_keyword(line, after_paren, len, "do")) {
        buf_append(out, line, i);
        buf_puts(out, "if ");
        buf_t cond_buf;
        if (!buf_init(&cond_buf, paren_end - paren_start + 16)) return false;
        transform_line_content(&cond_buf, line + paren_start + 1,
                               paren_end - paren_start - 1, in_long, long_level);
        buf_append(out, cond_buf.data, cond_buf.len);
        tlsf_free(s_tlsf, cond_buf.data);
        buf_puts(out, " then");
        /* Copy anything after 'do' on the same line */
        size_t after_do = after_paren + 2;
        if (after_do < len) {
            buf_append(out, line + after_do, len - after_do);
        }
        return true;
    }

    /* Reject if followed by expression-continuation operators —
       that means the outer parens are part of a larger condition, not short-form */
    if (match_keyword(line, after_paren, len, "or") ||
        match_keyword(line, after_paren, len, "and"))
        return false;

    /* Reject if followed by comparison/arithmetic operators: the parens are
       a sub-expression, e.g. if (x or 0) < 5 then */
    if (after_paren < len) {
        char nc = line[after_paren];
        if (nc == '<' || nc == '>' || nc == '=' || nc == '~' || nc == '!' ||
            nc == '+' || nc == '*' || nc == '/' || nc == '%' || nc == '^' ||
            (nc == '.' && after_paren + 1 < len && line[after_paren + 1] == '.'))
            return false;
        /* Minus: reject only if it looks like a binary operator (preceded by close paren) */
        if (nc == '-' && after_paren + 1 < len && line[after_paren + 1] != '-')
            return false;
    }

    /* It's short-form. There must be something after the paren (the statement). */
    if (after_paren >= len) return false;

    /* Emit leading whitespace + keyword */
    buf_append(out, line, i);
    buf_puts(out, keyword);
    buf_putc(out, ' ');

    /* Transform condition (handles != etc.) */
    buf_t cond_buf;
    if (!buf_init(&cond_buf, paren_end - paren_start + 16)) return false;
    transform_line_content(&cond_buf, line + paren_start + 1,
                           paren_end - paren_start - 1, in_long, long_level);
    buf_append(out, cond_buf.data, cond_buf.len);
    tlsf_free(s_tlsf, cond_buf.data);

    buf_putc(out, ' ');
    buf_puts(out, block_word);
    buf_putc(out, ' ');

    /* Find trailing comment in the statement body (-- not inside a string).
       We need to place 'end' BEFORE the comment, not after it. */
    size_t stmt_start = after_paren;
    size_t stmt_len = len - after_paren;
    size_t comment_pos = stmt_len; /* default: no comment */
    {
        size_t si = 0;
        while (si < stmt_len) {
            char sc = line[stmt_start + si];
            if (sc == '"' || sc == '\'') {
                si = skip_string(line + stmt_start, si, stmt_len);
                continue;
            }
            if (sc == '[') {
                int lvl = long_bracket_level(line + stmt_start, si, stmt_len);
                if (lvl >= 0) { si = skip_long_bracket(line + stmt_start, si + lvl + 2, stmt_len, lvl); continue; }
            }
            if (sc == '-' && si + 1 < stmt_len && line[stmt_start + si + 1] == '-') {
                comment_pos = si;
                break;
            }
            /* Also check for PICO-8 // comment */
            if (sc == '/' && si + 1 < stmt_len && line[stmt_start + si + 1] == '/') {
                comment_pos = si;
                break;
            }
            si++;
        }
    }

    /* Transform statement code (up to comment) */
    buf_t stmt_buf;
    if (!buf_init(&stmt_buf, stmt_len + 16)) return false;
    transform_line_content(&stmt_buf, line + stmt_start,
                           comment_pos, in_long, long_level);
    buf_append(out, stmt_buf.data, stmt_buf.len);
    tlsf_free(s_tlsf, stmt_buf.data);

    buf_putc(out, ' ');
    buf_puts(out, block_end);

    /* Append trailing comment if present */
    if (comment_pos < stmt_len) {
        buf_putc(out, ' ');
        /* Convert // to -- */
        if (line[stmt_start + comment_pos] == '/') {
            buf_puts(out, "--");
            buf_append(out, line + stmt_start + comment_pos + 2,
                       stmt_len - comment_pos - 2);
        } else {
            buf_append(out, line + stmt_start + comment_pos,
                       stmt_len - comment_pos);
        }
    }
    return true;
}

/* --- Compound assignment expansion --- */

/*
 * Scan backwards from pos to find the LHS of a compound assignment.
 * The LHS can be: identifier, table[expr], table.field, or chains thereof.
 * Returns the start index of the LHS, or pos if not found.
 */
static size_t find_lhs_start(const char *line, size_t pos) {
    if (pos == 0) return pos;
    size_t i = pos;

    /* Walk backwards over whitespace */
    while (i > 0 && (line[i-1] == ' ' || line[i-1] == '\t')) i--;
    if (i == 0) return pos;

    /* Walk backwards over the LHS expression */
    while (i > 0) {
        /* Check for ] — need to find matching [ */
        if (line[i-1] == ']') {
            int depth = 1;
            i--;
            while (i > 0 && depth > 0) {
                i--;
                if (line[i] == ']') depth++;
                else if (line[i] == '[') depth--;
            }
            /* Now at '[', continue to see what's before it */
            continue;
        }

        /* Identifier or dot.field */
        if (is_ident_char(line[i-1])) {
            while (i > 0 && is_ident_char(line[i-1])) i--;
            /* Check for dot before identifier (t.field) */
            if (i > 0 && line[i-1] == '.') {
                i--;
                continue;
            }
            break;
        }

        break;
    }

    return i;
}

/* --- Scan for compound assignment operator outside strings/comments --- */

/*
 * Find the first compound assignment operator (+=, -=, *=, /=, %=, ..=)
 * that is NOT inside a string literal or comment. Returns the index of the
 * operator character (e.g., '+' in '+='), or len if not found.
 * Sets *op_str to the operator string and *op_total_len to the total
 * operator+equals length (2 for +=, 3 for ..=).
 */
static size_t find_compound_op(const char *line, size_t len,
                               const char **op_str, size_t *op_total_len) {
    size_t i = 0;
    while (i < len) {
        char c = line[i];

        /* Skip string literals */
        if (c == '"' || c == '\'') {
            i = skip_string(line, i, len);
            continue;
        }

        /* Skip long strings */
        if (c == '[') {
            int lvl = long_bracket_level(line, i, len);
            if (lvl >= 0) {
                i = skip_long_bracket(line, i + lvl + 2, len, lvl);
                continue;
            }
        }

        /* Stop at Lua comments */
        if (c == '-' && i + 1 < len && line[i+1] == '-')
            break;

        /* Stop at PICO-8 // comments */
        if (c == '/' && i + 1 < len && line[i+1] == '/')
            break;

        /* Check for 3-char compound ops: ..=  ^^=  >>>=  <<=  >>= */
        if (c == '.' && i + 2 < len && line[i+1] == '.' && line[i+2] == '=') {
            *op_str = "..";
            *op_total_len = 3;
            return i;
        }
        if (c == '^' && i + 2 < len && line[i+1] == '^' && line[i+2] == '=') {
            *op_str = "~";   /* PICO-8 ^^ XOR → Lua 5.4 ~ */
            *op_total_len = 3;
            return i;
        }
        /* 4-char: >>>= (logical right shift assignment) */
        if (c == '>' && i + 3 < len && line[i+1] == '>' && line[i+2] == '>' && line[i+3] == '=') {
            *op_str = ">>";  /* PICO-8 >>> → Lua 5.4 >> */
            *op_total_len = 4;
            return i;
        }
        /* 3-char: <<= >>= */
        if (c == '<' && i + 2 < len && line[i+1] == '<' && line[i+2] == '=') {
            *op_str = "<<";
            *op_total_len = 3;
            return i;
        }
        if (c == '>' && i + 2 < len && line[i+1] == '>' && line[i+2] == '=') {
            *op_str = ">>";
            *op_total_len = 3;
            return i;
        }

        /* Check single-char compound ops: += -= *= /= %= \= */
        if (i + 1 < len && line[i+1] == '=') {
            if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '\\') {
                if (c == '\\')
                    *op_str = "//";  /* PICO-8 \ → Lua 5.4 // */
                else
                    *op_str = (c == '+') ? "+" :
                              (c == '-') ? "-" :
                              (c == '*') ? "*" :
                              (c == '/') ? "/" : "%";
                *op_total_len = 2;
                return i;
            }
        }

        i++;
    }
    return len;
}

/*
 * Find the end of the RHS expression for a compound assignment.
 * Stops at: bare Lua keywords (end, then, else, elseif, do) at depth 0,
 * or a new assignment pattern (identifier followed by = but not ==) at depth 0.
 * Returns the index where the RHS ends (exclusive).
 */
static size_t find_rhs_end(const char *line, size_t start, size_t len) {
    size_t i = start;
    int depth = 0; /* paren/bracket depth */
    while (i < len) {
        char c = line[i];

        /* Skip string literals */
        if (c == '"' || c == '\'') {
            i = skip_string(line, i, len);
            continue;
        }
        /* Skip long strings */
        if (c == '[') {
            int lvl = long_bracket_level(line, i, len);
            if (lvl >= 0) { i = skip_long_bracket(line, i + lvl + 2, len, lvl); continue; }
        }
        /* Stop at comments */
        if (c == '-' && i + 1 < len && line[i+1] == '-') break;
        if (c == '/' && i + 1 < len && line[i+1] == '/') break;

        /* Track depth */
        if (c == '(' || c == '[') { depth++; i++; continue; }
        if (c == ')' || c == ']') { if (depth > 0) depth--; i++; continue; }

        if (depth == 0) {
            /* Check for bare keywords that end a statement */
            if (is_ident_char(c) && (i == start || !is_ident_char(line[i-1]))) {
                if (match_keyword(line, i, len, "end") ||
                    match_keyword(line, i, len, "then") ||
                    match_keyword(line, i, len, "else") ||
                    match_keyword(line, i, len, "elseif") ||
                    match_keyword(line, i, len, "do")) {
                    break;
                }
            }

            /* Detect new assignment: at a space boundary, look ahead for
               ident = (but not ==). This catches patterns like "d=-1" where
               the space-separated token starts a new statement. */
            if (c == ' ' || c == '\t') {
                size_t j = i;
                while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
                if (j < len && is_ident_char(line[j])) {
                    /* Scan past identifier */
                    while (j < len && is_ident_char(line[j])) j++;
                    /* Skip whitespace between identifier and operator */
                    while (j < len && (line[j] == ' ' || line[j] == '\t')) j++;
                    /* Check for = (not ==, not <=, not >=, not ~=, not !=) */
                    if (j < len && line[j] == '=' && (j + 1 >= len || line[j+1] != '=')) {
                        break; /* RHS ends here at the whitespace */
                    }
                    /* Also check for compound assignment on the next token */
                    if (j + 1 < len && line[j+1] == '=' &&
                        (line[j] == '+' || line[j] == '-' || line[j] == '*' ||
                         line[j] == '/' || line[j] == '%')) {
                        break;
                    }
                    if (j + 2 < len && line[j] == '.' && line[j+1] == '.' && line[j+2] == '=') {
                        break;
                    }
                }
            }
        }

        i++;
    }
    /* Trim trailing whitespace from RHS */
    while (i > start && (line[i-1] == ' ' || line[i-1] == '\t')) i--;
    return i;
}

/*
 * Find where a line comment starts (-- or //) outside of string literals.
 * Returns the index of the comment start, or len if no comment found.
 */
static size_t find_comment_start(const char *line, size_t len) {
    size_t i = 0;
    while (i < len) {
        char c = line[i];
        if (c == '"' || c == '\'') { i = skip_string(line, i, len); continue; }
        if (c == '[') {
            int lvl = long_bracket_level(line, i, len);
            if (lvl >= 0) { i = skip_long_bracket(line, i + lvl + 2, len, lvl); continue; }
        }
        if (c == '-' && i + 1 < len && line[i+1] == '-') return i;
        if (c == '/' && i + 1 < len && line[i+1] == '/') return i;
        i++;
    }
    return len;
}

/*
 * Check whether the character at line[pos] is preceded (ignoring whitespace)
 * by a "value" token — identifier, digit, close paren/bracket, or close quote.
 * Used to disambiguate PICO-8 % (peek2 vs modulo).
 */
static bool is_value_before(const char *line, size_t pos) {
    while (pos > 0 && (line[pos-1] == ' ' || line[pos-1] == '\t')) pos--;
    if (pos == 0) return false;
    char prev = line[pos - 1];
    if (prev == ')' || prev == ']' || prev == '"' || prev == '\'') return true;
    if (prev >= '0' && prev <= '9') return true;
    if (!is_ident_char(prev)) return false;
    /* Previous char is ident — check if it's a keyword (keywords aren't values) */
    size_t end = pos;
    while (pos > 0 && is_ident_char(line[pos-1])) pos--;
    size_t wlen = end - pos;
    const char *w = line + pos;
    if ((wlen == 6 && memcmp(w, "return", 6) == 0) ||
        (wlen == 3 && memcmp(w, "and", 3) == 0) ||
        (wlen == 2 && memcmp(w, "or", 2) == 0) ||
        (wlen == 3 && memcmp(w, "not", 3) == 0) ||
        (wlen == 4 && memcmp(w, "then", 4) == 0) ||
        (wlen == 2 && memcmp(w, "do", 2) == 0) ||
        (wlen == 4 && memcmp(w, "else", 4) == 0) ||
        (wlen == 6 && memcmp(w, "elseif", 6) == 0) ||
        (wlen == 5 && memcmp(w, "local", 5) == 0) ||
        (wlen == 2 && memcmp(w, "in", 2) == 0) ||
        (wlen == 5 && memcmp(w, "until", 5) == 0) ||
        (wlen == 6 && memcmp(w, "repeat", 6) == 0) ||
        (wlen == 8 && memcmp(w, "function", 8) == 0))
        return false;
    return true;
}

/*
 * Find where a PICO-8 peek operand ends (@expr, %expr, $expr).
 * Operand: parenthesized expression, identifier (with . chains), or number.
 */
static size_t peek_operand_end(const char *line, size_t start, size_t len) {
    size_t i = start;
    if (i >= len) return start;

    /* Parenthesized expression */
    if (line[i] == '(') {
        size_t close = find_close_paren(line, i, len);
        return (close < len) ? close + 1 : len;
    }

    /* Hex number: 0x... */
    if (line[i] == '0' && i + 1 < len && (line[i+1] == 'x' || line[i+1] == 'X')) {
        i += 2;
        while (i < len && ((line[i] >= '0' && line[i] <= '9') ||
                           (line[i] >= 'a' && line[i] <= 'f') ||
                           (line[i] >= 'A' && line[i] <= 'F') ||
                           line[i] == '.')) i++;
        return i;
    }

    /* Decimal number */
    if (line[i] >= '0' && line[i] <= '9') {
        while (i < len && ((line[i] >= '0' && line[i] <= '9') || line[i] == '.')) i++;
        return i;
    }

    /* Identifier (with . member access chains) */
    if (is_ident_char(line[i])) {
        while (i < len && is_ident_char(line[i])) i++;
        while (i < len && line[i] == '.' && i + 1 < len && is_ident_char(line[i+1])) {
            i++;
            while (i < len && is_ident_char(line[i])) i++;
        }
        return i;
    }

    return start;
}

/* Forward declaration — transform_chars calls itself recursively for peek operands */
static bool transform_chars(buf_t *out, const char *line, size_t len,
                            bool *in_long, int *long_level);

/*
 * Emit a PICO-8 peek call: peek(operand), peek2(operand), or peek4(operand).
 * Advances *pi past the operand.
 */
static void emit_peek_call(buf_t *out, const char *func,
                           const char *line, size_t *pi, size_t len,
                           bool *in_long, int *long_level) {
    size_t i = *pi;
    buf_puts(out, func);
    buf_putc(out, '(');

    if (i < len && line[i] == '(') {
        /* Parenthesized: strip outer parens, transform content */
        size_t close = find_close_paren(line, i, len);
        if (close < len) {
            transform_chars(out, line + i + 1, close - i - 1, in_long, long_level);
            *pi = close + 1;
        } else {
            transform_chars(out, line + i, len - i, in_long, long_level);
            *pi = len;
        }
    } else {
        /* Identifier or number — find extent and transform */
        size_t end = peek_operand_end(line, i, len);
        transform_chars(out, line + i, end - i, in_long, long_level);
        *pi = end;
    }

    buf_putc(out, ')');
}

/* --- Character-by-character transform helper ---
 * Handles: string escapes, long strings, comments (-- and //),
 * != → ~=, >>> → >>, ^^ → ~, \ → //, @/%/$ peek operators,
 * P8SCII, binary literals.
 * Returns true if a long string/comment was opened (sets in_long/long_level).
 */
static bool transform_chars(buf_t *out, const char *line, size_t len,
                            bool *in_long, int *long_level) {
    size_t i = 0;
    while (i < len) {
        char c = line[i];

        /* String literals — copy with \^ escape conversion */
        if (c == '"' || c == '\'') {
            char q = c;
            buf_putc(out, c);
            i++;
            while (i < len) {
                if (line[i] == '\\' && i + 1 < len) {
                    if (line[i+1] == '^' && i + 2 < len) {
                        /* \^X → Lua numeric escape for P8SCII control code */
                        char hex = line[i+2];
                        int val = -1;
                        if (hex >= '0' && hex <= '9') val = hex - '0';
                        else if (hex >= 'a' && hex <= 'f') val = hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F') val = hex - 'A' + 10;
                        if (val >= 0) {
                            char esc[5];
                            snprintf(esc, sizeof(esc), "\\%d", val);
                            buf_puts(out, esc);
                            i += 3;
                            continue;
                        }
                        /* \^g-\^z and \^- \^+ \^# → two-byte P8SCII command:
                           byte 127 (prefix) followed by the command character */
                        if ((hex >= 'g' && hex <= 'z') ||
                            hex == '-' || hex == '+' || hex == '#' ||
                            hex == '*' || hex == '!' || hex == '|') {
                            buf_puts(out, "\\127");
                            buf_putc(out, hex);
                            i += 3;
                            continue;
                        }
                    }
                    /* Regular escape — copy both chars */
                    buf_putc(out, line[i]);
                    buf_putc(out, line[i+1]);
                    i += 2;
                    continue;
                }
                if (line[i] == q) {
                    buf_putc(out, line[i]);
                    i++;
                    break;
                }
                if (line[i] == '\n') break; /* unterminated */
                buf_putc(out, line[i]);
                i++;
            }
            continue;
        }

        /* Long strings [=*[ */
        if (c == '[') {
            int lvl = long_bracket_level(line, i, len);
            if (lvl >= 0) {
                size_t bracket_content = i + lvl + 2;
                size_t end = skip_long_bracket(line, bracket_content, len, lvl);
                if (end >= len) {
                    *in_long = true;
                    *long_level = lvl;
                    buf_append(out, line + i, len - i);
                    return true;
                }
                buf_append(out, line + i, end - i);
                i = end;
                continue;
            }
        }

        /* Lua -- line comment or --[=*[ long comment: copy rest of line */
        if (c == '-' && i + 1 < len && line[i+1] == '-') {
            if (i + 2 < len) {
                int lvl = long_bracket_level(line, i + 2, len);
                if (lvl >= 0) {
                    size_t bracket_content = i + 2 + lvl + 2;
                    size_t end = skip_long_bracket(line, bracket_content, len, lvl);
                    if (end >= len) {
                        *in_long = true;
                        *long_level = lvl;
                        buf_append(out, line + i, len - i);
                        return true;
                    }
                    buf_append(out, line + i, end - i);
                    i = end;
                    continue;
                }
            }
            buf_append(out, line + i, len - i);
            return true;
        }

        /* PICO-8 // line comment → -- */
        if (c == '/' && i + 1 < len && line[i+1] == '/') {
            buf_puts(out, "--");
            buf_append(out, line + i + 2, len - i - 2);
            return true;
        }

        /* != → ~= */
        if (c == '!' && i + 1 < len && line[i+1] == '=') {
            buf_puts(out, "~=");
            i += 2;
            continue;
        }

        /* P8SCII: multi-byte UTF-8 characters → _PG_XXXX identifiers */
        if ((unsigned char)c >= 0xC0) {
            uint32_t cp = 0;
            int nbytes = 0;
            if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; nbytes = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; nbytes = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; nbytes = 4; }
            else { buf_putc(out, c); i++; continue; }

            int valid = 1;
            for (int j = 1; j < nbytes; j++) {
                if (i + j >= len || ((unsigned char)line[i+j] & 0xC0) != 0x80) {
                    valid = 0; break;
                }
                cp = (cp << 6) | ((unsigned char)line[i+j] & 0x3F);
            }
            if (!valid) { buf_putc(out, c); i++; continue; }
            i += nbytes;

            /* Skip variation selector U+FE0F following this character */
            if (cp != 0xFE0F && i + 2 < len &&
                (unsigned char)line[i] == 0xEF &&
                (unsigned char)line[i+1] == 0xB8 &&
                (unsigned char)line[i+2] == 0x8F) {
                i += 3;
            }

            /* Drop standalone variation selector */
            if (cp == 0xFE0F) continue;

            char idbuf[16];
            snprintf(idbuf, sizeof(idbuf), "_PG_%X", (unsigned)cp);
            buf_puts(out, idbuf);
            continue;
        }

        /* Stray UTF-8 continuation bytes — skip */
        if ((unsigned char)c >= 0x80) {
            i++;
            continue;
        }

        /* 0b binary literals → numeric conversion */
        if (c == '0' && i + 2 < len &&
            (line[i+1] == 'b' || line[i+1] == 'B') &&
            (line[i+2] == '0' || line[i+2] == '1')) {
            /* Not part of an existing identifier */
            if (i > 0 && is_ident_char(line[i-1])) {
                buf_putc(out, c); i++; continue;
            }
            i += 2; /* skip '0b' */
            uint32_t int_val = 0;
            while (i < len && (line[i] == '0' || line[i] == '1')) {
                int_val = (int_val << 1) | (unsigned)(line[i] - '0');
                i++;
            }
            if (i < len && line[i] == '.' &&
                i + 1 < len && (line[i+1] == '0' || line[i+1] == '1')) {
                i++; /* skip '.' */
                double frac = 0.0;
                double weight = 0.5;
                while (i < len && (line[i] == '0' || line[i] == '1')) {
                    if (line[i] == '1') frac += weight;
                    weight *= 0.5;
                    i++;
                }
                char nbuf[32];
                snprintf(nbuf, sizeof(nbuf), "%.10g", (double)int_val + frac);
                buf_puts(out, nbuf);
            } else {
                char nbuf[16];
                snprintf(nbuf, sizeof(nbuf), "0x%x", (unsigned)int_val);
                buf_puts(out, nbuf);
            }
            continue;
        }

        /* PICO-8 >>> logical right shift → >> (Lua 5.4) */
        if (c == '>' && i + 2 < len && line[i+1] == '>' && line[i+2] == '>') {
            buf_puts(out, ">>");
            i += 3;
            continue;
        }

        /* PICO-8 ^^ XOR → ~ (Lua 5.4 bitwise XOR) */
        if (c == '^' && i + 1 < len && line[i+1] == '^') {
            buf_putc(out, '~');
            i += 2;
            continue;
        }

        /* PICO-8 \ integer division → // (Lua 5.4) */
        if (c == '\\') {
            buf_puts(out, "//");
            i++;
            continue;
        }

        /* PICO-8 @expr → peek(expr) */
        if (c == '@') {
            i++;
            emit_peek_call(out, "peek", line, &i, len, in_long, long_level);
            continue;
        }

        /* PICO-8 $expr → peek4(expr) */
        if (c == '$') {
            i++;
            emit_peek_call(out, "peek4", line, &i, len, in_long, long_level);
            continue;
        }

        /* PICO-8 %expr → peek2(expr) when unary; modulo when binary */
        if (c == '%' && !is_value_before(line, i)) {
            i++;
            emit_peek_call(out, "peek2", line, &i, len, in_long, long_level);
            continue;
        }

        /* Default: copy character */
        buf_putc(out, c);
        i++;
    }

    return false;
}

/* --- Main line content transform --- */

static bool transform_line_content(buf_t *out, const char *line, size_t len,
                                   bool *in_long, int *long_level) {
    /* First: check for ?expr at start of line → print(expr) */
    {
        size_t i = skip_ws(line, 0, len);
        if (i < len && line[i] == '?') {
            buf_append(out, line, i); /* preserve leading whitespace */
            buf_puts(out, "print(");
            size_t expr_start = i + 1;
            /* skip optional whitespace after ? */
            if (expr_start < len && line[expr_start] == ' ') expr_start++;
            /* Find trailing comment and exclude it from the print() arg */
            size_t comment_pos = find_comment_start(line + expr_start, len - expr_start);
            size_t expr_len = comment_pos;
            /* Trim trailing whitespace from expression */
            while (expr_len > 0 && (line[expr_start + expr_len - 1] == ' ' ||
                                     line[expr_start + expr_len - 1] == '\t'))
                expr_len--;
            buf_append(out, line + expr_start, expr_len);
            buf_puts(out, ")");
            /* Append the comment (transformed) if present */
            if (comment_pos < len - expr_start) {
                buf_putc(out, ' ');
                size_t cpos = expr_start + comment_pos;
                if (line[cpos] == '/' && cpos + 1 < len && line[cpos+1] == '/') {
                    buf_puts(out, "--");
                    buf_append(out, line + cpos + 2, len - cpos - 2);
                } else {
                    buf_append(out, line + cpos, len - cpos);
                }
            }
            return true;
        }
    }

    /* Check for short-form if/while */
    if (try_short_form(out, line, len, "if", "then", "end", in_long, long_level))
        return true;
    if (try_short_form(out, line, len, "while", "do", "end", in_long, long_level))
        return true;

    /* Check for compound assignment (line-level, before char-by-char) */
    {
        const char *op_str;
        size_t op_total_len;
        size_t op_pos = find_compound_op(line, len, &op_str, &op_total_len);
        if (op_pos < len) {
            size_t lhs_start = find_lhs_start(line, op_pos);
            if (lhs_start < op_pos) {
                /* LHS: line[lhs_start..op_pos), trimmed of trailing whitespace */
                size_t lhs_end = op_pos;
                while (lhs_end > lhs_start && (line[lhs_end-1] == ' ' || line[lhs_end-1] == '\t'))
                    lhs_end--;

                /* RHS: from after operator to boundary (keyword, new assignment, comment) */
                size_t rhs_start = op_pos + op_total_len;
                while (rhs_start < len && (line[rhs_start] == ' ' || line[rhs_start] == '\t'))
                    rhs_start++;
                size_t rhs_end = find_rhs_end(line, rhs_start, len);

                /* Emit: [prefix] LHS = LHS op (RHS) */
                transform_chars(out, line, lhs_start, in_long, long_level); /* prefix before LHS */
                buf_append(out, line + lhs_start, lhs_end - lhs_start);  /* LHS */
                buf_puts(out, " = ");
                buf_append(out, line + lhs_start, lhs_end - lhs_start);  /* LHS */
                buf_putc(out, ' ');
                buf_puts(out, op_str);
                buf_puts(out, " (");
                transform_chars(out, line + rhs_start, rhs_end - rhs_start, in_long, long_level); /* RHS */
                buf_puts(out, ")");

                /* Process remainder of line (may contain more statements, keywords) */
                if (rhs_end < len) {
                    /* Skip leading whitespace — we already broke at it */
                    size_t rem = rhs_end;
                    while (rem < len && (line[rem] == ' ' || line[rem] == '\t')) rem++;
                    if (rem < len) {
                        buf_putc(out, ' ');
                        return transform_line_content(out, line + rem, len - rem,
                                                      in_long, long_level);
                    }
                }
                return true;
            }
        }
    }

    /* Character-by-character transform for the rest */
    if (transform_chars(out, line, len, in_long, long_level))
        return true; /* long string/comment opened */

    return true;
}
