/*
 * test_p8_preprocess.c — Host-side test for PICO-8 preprocessor
 * Build: gcc -o test_p8_preprocess test_p8_preprocess.c p8_preprocess.c tlsf/tlsf.c -I.
 * Run:   ./test_p8_preprocess
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "p8_preprocess.h"

/* Provide a fake TLSF backed by standard malloc for host testing */
#include "tlsf/tlsf.h"

static char heap_mem[4 * 1024 * 1024]; /* 4MB test heap */

static int tests_run = 0;
static int tests_passed = 0;

static void test(const char *name, const char *input, const char *expected) {
    tests_run++;
    size_t out_len;
    char *result = p8_preprocess(input, strlen(input), &out_len);
    if (!result) {
        printf("FAIL [%s]: p8_preprocess returned NULL\n", name);
        return;
    }

    /* Trim trailing newline from result for comparison (preprocessor adds \n per line) */
    /* The expected string should match the output including added newlines */
    if (strcmp(result, expected) == 0) {
        tests_passed++;
        printf("PASS [%s]\n", name);
    } else {
        printf("FAIL [%s]\n", name);
        printf("  input:    |%s|\n", input);
        printf("  expected: |%s|\n", expected);
        printf("  got:      |%s|\n", result);
    }

    /* Can't easily free TLSF memory in this test since we don't expose tlsf handle,
       but it doesn't matter for a test. The pool is large enough. */
}

int main(void) {
    /* Init TLSF with our test heap */
    tlsf_t tlsf = tlsf_create_with_pool(heap_mem, sizeof(heap_mem));
    if (!tlsf) {
        fprintf(stderr, "Failed to create TLSF pool\n");
        return 1;
    }
    p8_preprocess_init(tlsf);

    printf("=== PICO-8 Preprocessor Tests ===\n\n");

    /* 1. != -> ~= */
    test("not-equal",
         "if x != 0 then y=1 end",
         "if x ~= 0 then y=1 end\n");

    /* 2. // comments -> -- */
    test("line-comment",
         "x = 1 // this is a comment",
         "x = 1 -- this is a comment\n");

    /* 3. Compound assignment += */
    test("plus-equals",
         "x += 1",
         "x = x + (1)\n");

    test("minus-equals",
         "score -= 10",
         "score = score - (10)\n");

    test("multiply-equals",
         "x *= 2",
         "x = x * (2)\n");

    test("divide-equals",
         "x /= 4",
         "x = x / (4)\n");

    test("modulo-equals",
         "x %= 3",
         "x = x % (3)\n");

    test("concat-equals",
         "s ..= \"hello\"",
         "s = s .. (\"hello\")\n");

    /* Compound with table LHS */
    test("table-plus-equals",
         "t[i] += 1",
         "t[i] = t[i] + (1)\n");

    test("dotfield-plus-equals",
         "obj.x += 5",
         "obj.x = obj.x + (5)\n");

    /* 4. ?expr -> print(expr) */
    test("print-shorthand",
         "?\"hello world\"",
         "print(\"hello world\")\n");

    test("print-shorthand-expr",
         "?x+1",
         "print(x+1)\n");

    test("print-shorthand-with-comment",
         "?\"hello\"..x  // expect 14",
         "print(\"hello\"..x) -- expect 14\n");

    test("print-shorthand-with-lua-comment",
         "?x+1  -- a comment",
         "print(x+1) -- a comment\n");

    /* 5. Short-form if */
    test("short-if",
         "if (x>0) print(x)",
         "if x>0 then print(x) end\n");

    /* Standard if should pass through */
    test("normal-if",
         "if x>0 then print(x) end",
         "if x>0 then print(x) end\n");

    /* Short-form if with != in condition */
    test("short-if-neq",
         "if (x != 0) print(x)",
         "if x ~= 0 then print(x) end\n");

    /* 6. Short-form while */
    test("short-while",
         "while (btn(0)) x+=1",
         "while btn(0) do x = x + (1) end\n");

    /* 7. Backslash line continuation */
    test("line-continuation",
         "x = 1 +\\\n2 + 3",
         "x = 1 + 2 + 3\n");

    /* String preservation */
    test("string-preserve-neq",
         "x = \"a != b\"",
         "x = \"a != b\"\n");

    test("string-preserve-comment",
         "x = \"a // b\"",
         "x = \"a // b\"\n");

    /* Long string preservation */
    test("long-string",
         "x = [[hello != world]]",
         "x = [[hello != world]]\n");

    /* Multi-line long string */
    test("multiline-long-string",
         "x = [[hello\n!= world]]",
         "x = [[hello\n!= world]]\n");

    /* -- comment should pass through */
    test("lua-comment-passthrough",
         "-- this is a lua comment",
         "-- this is a lua comment\n");

    /* Mixed != and // on same line */
    test("mixed-neq-comment",
         "if x != 0 then y=1 end // check",
         "if x ~= 0 then y=1 end -- check\n");

    /* Leading whitespace preserved */
    test("indent-preserved",
         "  x += 1",
         "  x = x + (1)\n");

    /* Passthrough for normal Lua */
    test("passthrough",
         "for i=1,10 do print(i) end",
         "for i=1,10 do print(i) end\n");

    /* --- P8SCII glyph conversion --- */

    /* ❎ (U+274E) = UTF-8 E2 9D 8E → _PG_274E */
    test("p8scii-btn-x",
         "btn(\xe2\x9d\x8e)",
         "btn(_PG_274E)\n");

    /* ⬅️ (U+2B05 + U+FE0F) = UTF-8 E2 AC 85 EF B8 8F → _PG_2B05 */
    test("p8scii-arrow-left",
         "btn(\xe2\xac\x85\xef\xb8\x8f)",
         "btn(_PG_2B05)\n");

    /* 🅾️ (U+1F17E + U+FE0F) = UTF-8 F0 9F 85 BE EF B8 8F → _PG_1F17E */
    test("p8scii-btn-o",
         "btn(\xf0\x9f\x85\xbe\xef\xb8\x8f)",
         "btn(_PG_1F17E)\n");

    /* ░ (U+2591) = UTF-8 E2 96 91 → _PG_2591 */
    test("p8scii-fillp",
         "fillp(\xe2\x96\x91)",
         "fillp(_PG_2591)\n");

    /* P8SCII in string should be preserved (not converted) */
    test("p8scii-in-string",
         "print(\"press \xe2\x9d\x8e to jump\")",
         "print(\"press \xe2\x9d\x8e to jump\")\n");

    /* --- 0b binary literal conversion --- */

    test("binary-literal",
         "x = 0b1010",
         "x = 0xa\n");

    test("binary-literal-long",
         "x = 0b0101101001011010",
         "x = 0x5a5a\n");

    test("binary-literal-frac",
         "x = 0b1010.1",
         "x = 10.5\n");

    /* 0b inside string — should NOT convert */
    test("binary-in-string",
         "x = \"0b1010\"",
         "x = \"0b1010\"\n");

    /* --- Compound assignment RHS boundary tests --- */

    /* RHS should stop at bare 'end' keyword */
    test("compound-rhs-end",
         "then pl.x += pl.dx end",
         "then pl.x = pl.x + (pl.dx) end\n");

    /* RHS should stop at new assignment on same line */
    test("compound-rhs-new-assign",
         "dx-= ac d=-1",
         "dx = dx - (ac) d=-1\n");

    /* RHS should stop at 'then' keyword */
    test("compound-rhs-then",
         "x += 1 then",
         "x = x + (1) then\n");

    /* wander.p8 line 34: short-form if with compound + new assignment */
    test("wander-line34",
         "if (btn(\xe2\xac\x85\xef\xb8\x8f)) dx-= ac d=-1",
         "if btn(_PG_2B05) then dx = dx - (ac) d=-1 end\n");

    /* wander.p8 line 35 */
    test("wander-line35",
         "if (btn(\xe2\x9e\xa1\xef\xb8\x8f)) dx+= ac d= 1",
         "if btn(_PG_27A1) then dx = dx + (ac) d= 1 end\n");

    /* --- Short-form if with 'or'/'and' continuation --- */

    /* if (cond) or ... should NOT be treated as short-form */
    test("if-or-continuation",
         "if (x>1) or",
         "if (x>1) or\n");

    test("if-and-continuation",
         "if (x>1) and",
         "if (x>1) and\n");

    /* Short-form if with compound assignment in body + end */
    test("short-if-compound-end",
         "if (c) x+=1 end",
         "if c then x = x + (1) end end\n");

    /* Short-form if with trailing comment — end must go BEFORE comment */
    test("short-if-trailing-comment",
         "if (j==2) hx -=.5 --hy-=.7",
         "if j==2 then hx = hx - (.5) end --hy-=.7\n");

    test("short-if-trailing-p8-comment",
         "if (j==3) hx +=.5 //hy-=.7",
         "if j==3 then hx = hx + (.5) end --hy-=.7\n");

    /* --- P8SCII \^ escape in strings --- */

    /* \^c color escape → Lua numeric escape */
    test("p8scii-caret-color",
         "x = \"\\^1hello\"",
         "x = \"\\1hello\"\n");

    test("p8scii-caret-hex",
         "x = \"\\^ahello\"",
         "x = \"\\10hello\"\n");

    test("p8scii-caret-zero",
         "x = \"\\^0test\"",
         "x = \"\\0test\"\n");

    /* \^g-\^z → two-byte P8SCII command (byte 127 prefix + command char) */
    test("p8scii-caret-command-z",
         "x = \"\\^zhello\"",
         "x = \"\\127zhello\"\n");

    test("p8scii-caret-command-h",
         "x = \"\\^hhello\"",
         "x = \"\\127hhello\"\n");

    /* \^- and \^+ cursor commands */
    test("p8scii-caret-dash",
         "x = \"\\^-hello\"",
         "x = \"\\127-hello\"\n");

    test("p8scii-caret-plus",
         "x = \"\\^+hello\"",
         "x = \"\\127+hello\"\n");

    /* \^ with unknown char — should NOT be converted */
    test("p8scii-caret-unknown",
         "x = \"\\^(hello\"",
         "x = \"\\^(hello\"\n");

    /* --- != in prefix of compound assignment (celeste2.p8 bug) --- */

    /* The core bug: != in prefix before += was not converted to ~= */
    test("neq-in-compound-prefix",
         "if level_index != 8 then frames += 1 end",
         "if level_index ~= 8 then frames = frames + (1) end\n");

    /* Binary literal in RHS of compound assignment */
    test("binary-in-compound-rhs",
         "a += 0b1010",
         "a = a + (0xa)\n");

    /* P8-style comment in remainder after compound assignment */
    test("p8comment-after-compound",
         "x += 1 // comment",
         "x = x + (1) -- comment\n");

    /* No prefix, just plain compound — still works */
    test("compound-no-prefix",
         "y -= 1",
         "y = y - (1)\n");

    /* RHS should stop at spaced assignment: "seconds += 1 frames = 0" */
    test("compound-rhs-spaced-assign",
         "if frames == 30 then seconds += 1 frames = 0 end",
         "if frames == 30 then seconds = seconds + (1) frames = 0 end\n");

    /* --- PICO-8 operator conversions --- */

    /* >>> logical right shift → >> */
    test("lshr-operator",
         "x = a >>> b",
         "x = a >> b\n");

    /* ^^ XOR → ~ */
    test("xor-operator",
         "x = a ^^ b",
         "x = a ~ b\n");

    /* \ integer division → // */
    test("intdiv-operator",
         "y += x\\w",
         "y = y + (x//w)\n");

    /* @expr → peek(expr) */
    test("peek-ident",
         "x = @addr",
         "x = peek(addr)\n");

    /* @(expr) → peek(expr) */
    test("peek-paren",
         "x = @(addr + 1)",
         "x = peek(addr + 1)\n");

    /* @0x5f28 → peek(0x5f28) */
    test("peek-hex",
         "x = @0x5f28",
         "x = peek(0x5f28)\n");

    /* %expr → peek2(expr) when unary */
    test("peek2-ident",
         "x = %src",
         "x = peek2(src)\n");

    /* % as modulo when binary */
    test("modulo-binary",
         "x = a % b",
         "x = a % b\n");

    /* $expr → peek4(expr) */
    test("peek4-ident",
         "x = $addr",
         "x = peek4(addr)\n");

    /* Combined: cache += %src >>> 16 */
    test("compound-peek2-lshr",
         "cache+=%src>>>16",
         "cache = cache + (peek2(src)>>16)\n");

    /* \= compound assignment */
    test("intdiv-assign",
         "x \\= 2",
         "x = x // (2)\n");

    /* ^^= compound assignment */
    test("xor-assign",
         "x ^^= 0xff",
         "x = x ~ (0xff)\n");

    printf("\n=== Results: %d/%d passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
