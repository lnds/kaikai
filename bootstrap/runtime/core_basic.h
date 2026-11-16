/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- to-string ---------- */

static KAI_RC_NOINLINE KaiValue *kai_string_concat(KaiValue *a, KaiValue *b);

#if !KAI_SCHED_DECL_ONLY
/* 10^e * 2^(125 - floor(e * log2 10)) as a 126-bit word pair, rounded up unless
 * exact: the top bits of a base power of five times a small power, plus a
 * correction of 0 to 3 kept two bits per exponent. */
static const uint64_t kai_real_pow5_up[13][2] = {
    {0x2000000000000000, 0x0000000000000000},
    {0x295be96e64066972, 0x0000000000000000},
    {0x35748e292afa601a, 0x1ca924116635b620},
    {0x228b6fc50b7f31ea, 0xdb8d5a4c9b1e10cd},
    {0x2ca5dfb8c03143f9, 0xd63b7b247b0b2d95},
    {0x39b4c40ab65b3b07, 0x69837016455d2479},
    {0x254aad173ea5e82c, 0xb765169d1efc9860},
    {0x3032ca2a63f3cf1f, 0xe115da86ed05a9f7},
    {0x3e4b830d4de09851, 0x69dc2695b337e2a0},
    {0x2841d689391085cc, 0x2c933d96e184be07},
    {0x3407fbc42995e10b, 0x0b4add5a6c10cb61},
    {0x219fd66a752fb5b0, 0x127b63aaf3331217},
    {0x2b755e89f4a4ce7d, 0x9e7176e6bc7e6d58}
};
static const uint64_t kai_real_pow5_down[13][2] = {
    {0x2000000000000000, 0x0000000000000000},
    {0x318481895d962776, 0xa54d92bf80caa067},
    {0x264ff8b1b41edfea, 0xf951aa00e3bf900a},
    {0x3b4919c8d1cf8e04, 0xca4048fa6aac8eda},
    {0x2ddeb68185f8eef2, 0xc2739baed005cdd1},
    {0x237d7beaf165e723, 0xf2a34ffe87bd18f0},
    {0x36eb1b091f58a960, 0xf7d17dd1add2afce},
    {0x2a7db4c280e3476f, 0x17a7f3d3334847d3},
    {0x20e037aa4f692f18, 0x1091f2e7967dc79b},
    {0x32df7737689b689a, 0x2a650bd773df7f42},
    {0x275c6b23eb69b26d, 0x5bddcff0d80f6d2a},
    {0x3ce8809e7b55b522, 0x9ad0d49d5e304443},
    {0x2f201d49fb4f84af, 0x3181420dc13d7b3c}
};
static const uint64_t kai_real_pow5_fix[20] = {
    0x59a9a65665aa6aaa,
    0x5655a65656565a55,
    0x959baabaaaaaaa99,
    0x699a99559956595a,
    0xaebaaaab9a556955,
    0x659559599659aaaa,
    0xe9aaaaaaaaaaa995,
    0x55555a6eaaabdaaa,
    0x0000000005555555,
    0x0000000000000000,
    0x596695a595965800,
    0xa55969655969956a,
    0x5595559596555665,
    0x5656aa9969555655,
    0x659596959a555565,
    0xa955555555555555,
    0x69a56596aa9aa559,
    0x569a565659565695,
    0xa6a995a955a96656,
    0x0000000000029959
};
static const uint64_t kai_real_pow5_small[26] = {
    1ULL, 5ULL, 25ULL, 125ULL, 625ULL, 3125ULL, 15625ULL, 78125ULL, 390625ULL, 1953125ULL,
    9765625ULL, 48828125ULL, 244140625ULL, 1220703125ULL, 6103515625ULL, 30517578125ULL,
    152587890625ULL, 762939453125ULL, 3814697265625ULL, 19073486328125ULL, 95367431640625ULL,
    476837158203125ULL, 2384185791015625ULL, 11920928955078125ULL, 59604644775390625ULL,
    298023223876953125ULL};

static void kai_real_pow10(int e, uint64_t *g) {
    const uint64_t *base;
    int b;
    if (e >= 0) {
        base = kai_real_pow5_up[e / 26];
        b = e % 26;
    } else {
        int a = (-e + 25) / 26;
        base = kai_real_pow5_down[a];
        b = 26 * a + e;
    }
    unsigned __int128 lo = (unsigned __int128) base[1] * kai_real_pow5_small[b];
    unsigned __int128 hi = (unsigned __int128) base[0] * kai_real_pow5_small[b] + (lo >> 64);
    uint64_t top = (uint64_t) (hi >> 64);
    int bits = top ? 128 + 64 - __builtin_clzll(top) : 64 + 64 - __builtin_clzll((uint64_t) hi);
    int s = bits - 126;
    unsigned __int128 r = (hi << (64 - s)) | ((uint64_t) lo >> s);
    int i = 324 - e;
    r += (kai_real_pow5_fix[i / 32] >> (2 * (i % 32))) & 3;
    g[0] = (uint64_t) (r >> 64);
    g[1] = (uint64_t) r;
}

static uint64_t kai_real_round_odd(const uint64_t *g, uint64_t cp) {
    unsigned __int128 p1 = (unsigned __int128) cp * g[1];
    unsigned __int128 t = (unsigned __int128) cp * g[0] + (p1 >> 64);
    unsigned __int128 low = t & (((unsigned __int128) 1 << 65) - 1);
    return (uint64_t) (t >> 65) | (uint64_t) (low != 0 || (uint64_t) p1 != 0);
}

/* The scaled interval edge `m` * 2^(`q`-1) when it lands exactly on a
 * decimal grid point, which the round-to-odd product cannot tell from a
 * point just above it. */
static uint64_t kai_real_edge(uint64_t m, int q, int k, uint64_t approx) {
    if (k < 1 || k > 23 || q - 1 - k < 0 || m % kai_real_pow5_small[k] != 0) return approx;
    return ((m / kai_real_pow5_small[k]) << (q - 1 - k)) << 2;
}

/* Shortest round-trip digits of a finite nonzero Real as `*d` * 10^`*k`
 * (Schubfach). Returns 0 for subnormals and for a binade's lowest mantissa:
 * printing rounds to nearest at each precision, which on that asymmetric
 * interval is not always the shortest text. */
static int kai_real_shortest(double x, uint64_t *d, int *k) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    int bexp = (int) ((bits >> 52) & 0x7ff);
    if (bexp == 0) return 0;
    uint64_t c = bits & (((uint64_t) 1 << 52) - 1);
    c |= (uint64_t) 1 << 52;
    int q = bexp - 1075;
    if (c == ((uint64_t) 1 << 52)) return 0;
    int dk = (int) (((int64_t) q * 323228497) >> 30);
    int h = q + (int) (((int64_t) -dk * 3566893132) >> 30) + 4;
    uint64_t g[2];
    kai_real_pow10(-dk, g);
    uint64_t out = c & 1, cb = c << 2;
    uint64_t vb = kai_real_round_odd(g, cb << h);
    uint64_t vbl = kai_real_round_odd(g, (cb - 2) << h);
    uint64_t vbr = kai_real_round_odd(g, (cb + 2) << h);
    vbl = kai_real_edge(2 * c - 1, q, dk, vbl);
    vbr = kai_real_edge(2 * c + 1, q, dk, vbr);
    uint64_t s = vb >> 2;
    uint64_t lo10 = 10 * (s / 10), hi10 = lo10 + 10;
    int lo_in = vbl + out <= lo10 << 2, hi_in = (hi10 << 2) + out <= vbr;
    if (lo_in != hi_in) {
        *d = lo_in ? lo10 : hi10;
    } else {
        uint64_t t = s + 1;
        int s_in = vbl + out <= s << 2, t_in = (t << 2) + out <= vbr;
        int64_t cmp = (int64_t) (vb - ((s + t) << 1));
        *d = s_in != t_in ? (s_in ? s : t) : (cmp < 0 || (cmp == 0 && !(s & 1))) ? s : t;
    }
    *k = dk;
    return 1;
}

/* Writes the `%g`-shaped text of digits `d` * 10^`k` as printf would at
 * precision max(digits, 6). */
static void kai_real_layout(char *buf, int neg, uint64_t d, int k) {
    while (d % 10 == 0) { d /= 10; k++; }
    char ds[24];
    int n = 0;
    for (uint64_t r = d; r; r /= 10) ds[n++] = (char) ('0' + r % 10);
    for (int i = 0; i < n / 2; i++) { char t = ds[i]; ds[i] = ds[n - 1 - i]; ds[n - 1 - i] = t; }
    int e = k + n - 1, p = n < 6 ? 6 : n;
    char *o = buf;
    if (neg) *o++ = '-';
    if (e < -4 || e >= p) {
        *o++ = ds[0];
        if (n > 1) { *o++ = '.'; memcpy(o, ds + 1, (size_t) (n - 1)); o += n - 1; }
        o += sprintf(o, "e%c%02d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e >= 0) {
        int ip = e + 1 < n ? e + 1 : n;
        memcpy(o, ds, (size_t) ip); o += ip;
        for (int i = n; i < e + 1; i++) *o++ = '0';
        if (n > e + 1) { *o++ = '.'; memcpy(o, ds + e + 1, (size_t) (n - e - 1)); o += n - e - 1; }
        *o = 0;
    } else {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -e - 1; i++) *o++ = '0';
        memcpy(o, ds, (size_t) n); o += n; *o = 0;
    }
}

static int kai_real_reads_back(char *buf, size_t cap, int digits, double x) {
    snprintf(buf, cap, "%.*g", digits, x);
    return strtod(buf, NULL) == x;
}
#endif

/* A Real's text: the shortest `%.{p}g`, p from 6 to 17, that reads back
 * as the same number. A non-finite Real has one spelling. Defined once in
 * the runtime owner: a program only sees the prototype. */
KAI_SCHED_FN void kai_real_text(char *buf, size_t cap, double x)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    uint64_t d;
    int k;
    if (isfinite(x) && x != 0 && kai_real_shortest(x, &d, &k)) {
        kai_real_layout(buf, signbit(x), d, k);
        return;
    }
    snprintf(buf, cap, "%g", x);
    if (!isfinite(x) || strtod(buf, NULL) == x) return;
    for (int p = 7; p < 17 && !kai_real_reads_back(buf, cap, p, x); p++) { }
    if (strtod(buf, NULL) != x) snprintf(buf, cap, "%.17g", x);
}
#endif

KAI_RT_SHARED KaiValue *kai_to_string(KaiValue *v);

/* Encode scalar value `cp` into `out` (>= 4 bytes), the exact inverse of
 * the `string_cp_at` decode. Returns the byte width 1..4. A `Char` is
 * always a valid scalar value (enforced at `int_to_char`); the surrogate
 * / out-of-range clamp to U+FFFD only guards a raw uint32 reaching here. */
static int kai_utf8_encode(uint32_t cp, unsigned char *out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        out[0] = (unsigned char) cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char) (0xC0 | (cp >> 6));
        out[1] = (unsigned char) (0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char) (0xE0 | (cp >> 12));
        out[1] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char) (0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char) (0xF0 | (cp >> 18));
    out[1] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char) (0x80 | (cp & 0x3F));
    return 4;
}

/* The 128 one-byte Strings are immortal static cells over a static text
 * table: rendering an ASCII Char never allocates and the table is read-only
 * from process start. Cells are per-TU: Strings compare by content. */
#define KAI_ASCII_TEXT(n) (char) (n), '\0'
static char kai_ascii_text[256] = {
    KAI_ASCII_TEXT(0), KAI_ASCII_TEXT(1), KAI_ASCII_TEXT(2), KAI_ASCII_TEXT(3), KAI_ASCII_TEXT(4), KAI_ASCII_TEXT(5), KAI_ASCII_TEXT(6), KAI_ASCII_TEXT(7),
    KAI_ASCII_TEXT(8), KAI_ASCII_TEXT(9), KAI_ASCII_TEXT(10), KAI_ASCII_TEXT(11), KAI_ASCII_TEXT(12), KAI_ASCII_TEXT(13), KAI_ASCII_TEXT(14), KAI_ASCII_TEXT(15),
    KAI_ASCII_TEXT(16), KAI_ASCII_TEXT(17), KAI_ASCII_TEXT(18), KAI_ASCII_TEXT(19), KAI_ASCII_TEXT(20), KAI_ASCII_TEXT(21), KAI_ASCII_TEXT(22), KAI_ASCII_TEXT(23),
    KAI_ASCII_TEXT(24), KAI_ASCII_TEXT(25), KAI_ASCII_TEXT(26), KAI_ASCII_TEXT(27), KAI_ASCII_TEXT(28), KAI_ASCII_TEXT(29), KAI_ASCII_TEXT(30), KAI_ASCII_TEXT(31),
    KAI_ASCII_TEXT(32), KAI_ASCII_TEXT(33), KAI_ASCII_TEXT(34), KAI_ASCII_TEXT(35), KAI_ASCII_TEXT(36), KAI_ASCII_TEXT(37), KAI_ASCII_TEXT(38), KAI_ASCII_TEXT(39),
    KAI_ASCII_TEXT(40), KAI_ASCII_TEXT(41), KAI_ASCII_TEXT(42), KAI_ASCII_TEXT(43), KAI_ASCII_TEXT(44), KAI_ASCII_TEXT(45), KAI_ASCII_TEXT(46), KAI_ASCII_TEXT(47),
    KAI_ASCII_TEXT(48), KAI_ASCII_TEXT(49), KAI_ASCII_TEXT(50), KAI_ASCII_TEXT(51), KAI_ASCII_TEXT(52), KAI_ASCII_TEXT(53), KAI_ASCII_TEXT(54), KAI_ASCII_TEXT(55),
    KAI_ASCII_TEXT(56), KAI_ASCII_TEXT(57), KAI_ASCII_TEXT(58), KAI_ASCII_TEXT(59), KAI_ASCII_TEXT(60), KAI_ASCII_TEXT(61), KAI_ASCII_TEXT(62), KAI_ASCII_TEXT(63),
    KAI_ASCII_TEXT(64), KAI_ASCII_TEXT(65), KAI_ASCII_TEXT(66), KAI_ASCII_TEXT(67), KAI_ASCII_TEXT(68), KAI_ASCII_TEXT(69), KAI_ASCII_TEXT(70), KAI_ASCII_TEXT(71),
    KAI_ASCII_TEXT(72), KAI_ASCII_TEXT(73), KAI_ASCII_TEXT(74), KAI_ASCII_TEXT(75), KAI_ASCII_TEXT(76), KAI_ASCII_TEXT(77), KAI_ASCII_TEXT(78), KAI_ASCII_TEXT(79),
    KAI_ASCII_TEXT(80), KAI_ASCII_TEXT(81), KAI_ASCII_TEXT(82), KAI_ASCII_TEXT(83), KAI_ASCII_TEXT(84), KAI_ASCII_TEXT(85), KAI_ASCII_TEXT(86), KAI_ASCII_TEXT(87),
    KAI_ASCII_TEXT(88), KAI_ASCII_TEXT(89), KAI_ASCII_TEXT(90), KAI_ASCII_TEXT(91), KAI_ASCII_TEXT(92), KAI_ASCII_TEXT(93), KAI_ASCII_TEXT(94), KAI_ASCII_TEXT(95),
    KAI_ASCII_TEXT(96), KAI_ASCII_TEXT(97), KAI_ASCII_TEXT(98), KAI_ASCII_TEXT(99), KAI_ASCII_TEXT(100), KAI_ASCII_TEXT(101), KAI_ASCII_TEXT(102), KAI_ASCII_TEXT(103),
    KAI_ASCII_TEXT(104), KAI_ASCII_TEXT(105), KAI_ASCII_TEXT(106), KAI_ASCII_TEXT(107), KAI_ASCII_TEXT(108), KAI_ASCII_TEXT(109), KAI_ASCII_TEXT(110), KAI_ASCII_TEXT(111),
    KAI_ASCII_TEXT(112), KAI_ASCII_TEXT(113), KAI_ASCII_TEXT(114), KAI_ASCII_TEXT(115), KAI_ASCII_TEXT(116), KAI_ASCII_TEXT(117), KAI_ASCII_TEXT(118), KAI_ASCII_TEXT(119),
    KAI_ASCII_TEXT(120), KAI_ASCII_TEXT(121), KAI_ASCII_TEXT(122), KAI_ASCII_TEXT(123), KAI_ASCII_TEXT(124), KAI_ASCII_TEXT(125), KAI_ASCII_TEXT(126), KAI_ASCII_TEXT(127)
};
#define KAI_ASCII_CELL(n) { .rc = INT32_MAX, .tag = KAI_STR, \
    .as = { .s = { .len = 1, .bytes = &kai_ascii_text[2 * (n)] } } }
static KaiValue kai_ascii_strings[128] = {
    KAI_ASCII_CELL(0), KAI_ASCII_CELL(1), KAI_ASCII_CELL(2), KAI_ASCII_CELL(3), KAI_ASCII_CELL(4), KAI_ASCII_CELL(5), KAI_ASCII_CELL(6), KAI_ASCII_CELL(7),
    KAI_ASCII_CELL(8), KAI_ASCII_CELL(9), KAI_ASCII_CELL(10), KAI_ASCII_CELL(11), KAI_ASCII_CELL(12), KAI_ASCII_CELL(13), KAI_ASCII_CELL(14), KAI_ASCII_CELL(15),
    KAI_ASCII_CELL(16), KAI_ASCII_CELL(17), KAI_ASCII_CELL(18), KAI_ASCII_CELL(19), KAI_ASCII_CELL(20), KAI_ASCII_CELL(21), KAI_ASCII_CELL(22), KAI_ASCII_CELL(23),
    KAI_ASCII_CELL(24), KAI_ASCII_CELL(25), KAI_ASCII_CELL(26), KAI_ASCII_CELL(27), KAI_ASCII_CELL(28), KAI_ASCII_CELL(29), KAI_ASCII_CELL(30), KAI_ASCII_CELL(31),
    KAI_ASCII_CELL(32), KAI_ASCII_CELL(33), KAI_ASCII_CELL(34), KAI_ASCII_CELL(35), KAI_ASCII_CELL(36), KAI_ASCII_CELL(37), KAI_ASCII_CELL(38), KAI_ASCII_CELL(39),
    KAI_ASCII_CELL(40), KAI_ASCII_CELL(41), KAI_ASCII_CELL(42), KAI_ASCII_CELL(43), KAI_ASCII_CELL(44), KAI_ASCII_CELL(45), KAI_ASCII_CELL(46), KAI_ASCII_CELL(47),
    KAI_ASCII_CELL(48), KAI_ASCII_CELL(49), KAI_ASCII_CELL(50), KAI_ASCII_CELL(51), KAI_ASCII_CELL(52), KAI_ASCII_CELL(53), KAI_ASCII_CELL(54), KAI_ASCII_CELL(55),
    KAI_ASCII_CELL(56), KAI_ASCII_CELL(57), KAI_ASCII_CELL(58), KAI_ASCII_CELL(59), KAI_ASCII_CELL(60), KAI_ASCII_CELL(61), KAI_ASCII_CELL(62), KAI_ASCII_CELL(63),
    KAI_ASCII_CELL(64), KAI_ASCII_CELL(65), KAI_ASCII_CELL(66), KAI_ASCII_CELL(67), KAI_ASCII_CELL(68), KAI_ASCII_CELL(69), KAI_ASCII_CELL(70), KAI_ASCII_CELL(71),
    KAI_ASCII_CELL(72), KAI_ASCII_CELL(73), KAI_ASCII_CELL(74), KAI_ASCII_CELL(75), KAI_ASCII_CELL(76), KAI_ASCII_CELL(77), KAI_ASCII_CELL(78), KAI_ASCII_CELL(79),
    KAI_ASCII_CELL(80), KAI_ASCII_CELL(81), KAI_ASCII_CELL(82), KAI_ASCII_CELL(83), KAI_ASCII_CELL(84), KAI_ASCII_CELL(85), KAI_ASCII_CELL(86), KAI_ASCII_CELL(87),
    KAI_ASCII_CELL(88), KAI_ASCII_CELL(89), KAI_ASCII_CELL(90), KAI_ASCII_CELL(91), KAI_ASCII_CELL(92), KAI_ASCII_CELL(93), KAI_ASCII_CELL(94), KAI_ASCII_CELL(95),
    KAI_ASCII_CELL(96), KAI_ASCII_CELL(97), KAI_ASCII_CELL(98), KAI_ASCII_CELL(99), KAI_ASCII_CELL(100), KAI_ASCII_CELL(101), KAI_ASCII_CELL(102), KAI_ASCII_CELL(103),
    KAI_ASCII_CELL(104), KAI_ASCII_CELL(105), KAI_ASCII_CELL(106), KAI_ASCII_CELL(107), KAI_ASCII_CELL(108), KAI_ASCII_CELL(109), KAI_ASCII_CELL(110), KAI_ASCII_CELL(111),
    KAI_ASCII_CELL(112), KAI_ASCII_CELL(113), KAI_ASCII_CELL(114), KAI_ASCII_CELL(115), KAI_ASCII_CELL(116), KAI_ASCII_CELL(117), KAI_ASCII_CELL(118), KAI_ASCII_CELL(119),
    KAI_ASCII_CELL(120), KAI_ASCII_CELL(121), KAI_ASCII_CELL(122), KAI_ASCII_CELL(123), KAI_ASCII_CELL(124), KAI_ASCII_CELL(125), KAI_ASCII_CELL(126), KAI_ASCII_CELL(127)
};

/* A Char as its UTF-8 text. */
static KaiValue *kai_char_to_string(uint32_t cp) {
    unsigned char buf[4];
    int n = kai_utf8_encode(cp, buf);
    if (n == 1) return &kai_ascii_strings[buf[0]];
    return kai_str_from_bytes((const char *) buf, (size_t) n);
}

KAI_RT_SHARED KaiValue *kai_list_to_string(KaiValue *v) {
    KaiValue *acc = kai_str("[");
    int first = 1;
    KaiSeqIt it;
    kai_seq_it_init(&it, v);
    for (KaiValue *x = kai_seq_it_next(&it); x; x = kai_seq_it_next(&it)) {
        if (!first) {
            KaiValue *c = kai_string_concat(acc, kai_str(", "));
            kai_decref(acc); acc = c;
        }
        first = 0;
        KaiValue *s = kai_to_string(x);
        KaiValue *c = kai_string_concat(acc, s);
        kai_decref(acc); kai_decref(s); acc = c;
        kai_decref(x);
    }
    KaiValue *c = kai_string_concat(acc, kai_str("]"));
    kai_decref(acc); return c;
}

KAI_RT_SHARED KaiValue *kai_to_string(KaiValue *v) {
    if (!v) return kai_str("<null>");
    char buf[64];
    /* Koka tagged-Int: a small Int is an immediate, not a heap value —
     * `v->tag` would dereference the fake pointer and crash. Render it
     * as the decoded integer before touching the header. */
    if (kai_is_value(v)) {
        snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
        return kai_str_dyn(buf);
    }
    switch ((KaiTag) v->tag) {
        case KAI_UNIT: return kai_str("()");
        case KAI_BOOL: return kai_str(v->as.b ? "true" : "false");
        case KAI_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
            return kai_str_dyn(buf);
        case KAI_REAL:
            kai_real_text(buf, sizeof(buf), v->as.r);
            return kai_str_dyn(buf);
        case KAI_CHAR: return kai_char_to_string(v->as.c);
        case KAI_STR:    return kai_incref(v);
        case KAI_NIL:    return kai_str("[]");
        case KAI_CONS:   return kai_list_to_string(v);
        case KAI_RANGE:  return kai_list_to_string(v);
        case KAI_VARIANT: {
            KaiValue *acc = kai_str(kai_variant_name_of(v->variant_tag));
            if (v->var_n_args > 0) {
                KaiValue *lp = kai_string_concat(acc, kai_str("(")); kai_decref(acc); acc = lp;
                /* Phase 2: primitive slots are boxed on-the-fly via
                 * kai_variant_slot_box; for the mask==0 hot path it
                 * just returns the stored pointer. */
                for (int i = 0; i < v->var_n_args; ++i) {
                    if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                    uint32_t k = kai_var_slot_kind(kai_slot_mask_of(v->variant_tag), i);
                    KaiValue *s;
                    if (k == KAI_VAR_SLOT_PTR) {
                        s = kai_to_string(kai_var_slots(v)[i].ptr);
                    } else {
                        KaiValue *tmp = kai_variant_slot_box(v, i);
                        s = kai_to_string(tmp);
                        kai_decref(tmp);
                    }
                    KaiValue *c = kai_string_concat(acc, s); kai_decref(acc); kai_decref(s); acc = c;
                }
                KaiValue *rp = kai_string_concat(acc, kai_str(")")); kai_decref(acc); acc = rp;
            }
            return acc;
        }
        case KAI_RECORD: {
            KaiValue *acc = kai_str("{");
            for (int i = 0; i < v->as.rec.n_fields; ++i) {
                if (i) { KaiValue *sep = kai_string_concat(acc, kai_str(", ")); kai_decref(acc); acc = sep; }
                KaiValue *nm = kai_str(v->as.rec.names[i]);
                KaiValue *a1 = kai_string_concat(acc, nm); kai_decref(acc); kai_decref(nm); acc = a1;
                KaiValue *a2 = kai_string_concat(acc, kai_str(": ")); kai_decref(acc); acc = a2;
                KaiValue *fs = kai_to_string(v->as.rec.fields[i]);
                KaiValue *a3 = kai_string_concat(acc, fs); kai_decref(acc); kai_decref(fs); acc = a3;
            }
            KaiValue *cl = kai_string_concat(acc, kai_str("}")); kai_decref(acc);
            return cl;
        }
        case KAI_CLOSURE: return kai_str("<closure>");
        case KAI_ARRAY:   return kai_str("<array>");
        case KAI_VEC:     return kai_str("<vec>");
        case KAI_REF: {
            /* Honest render `Ref(<inner>)` — closes the follow-up the
             * #257 retro left open (length-1 array printed as <array>). */
            KaiValue *inner = kai_to_string(v->as.ref.cell);
            KaiValue *open  = kai_string_concat(kai_str("Ref("), inner);
            KaiValue *full  = kai_string_concat(open, kai_str(")"));
            kai_decref(inner); kai_decref(open);
            return full;
        }
        case KAI_FIBER:   return kai_str("<fiber>");
        case KAI_PID:     return kai_str("<pid>");
        case KAI_FOREIGN: return kai_str("<foreign>");
        case KAI_CONT:    return kai_str("<continuation>");
        case KAI_SEQ:     return kai_str("<seq>");
        case KAI_BYTE:                                       /* Lane 4 (#473) */
            snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
            return kai_str_dyn(buf);
        case KAI_INT32:
            snprintf(buf, sizeof(buf), "%d", (int) v->as.i32);
            return kai_str_dyn(buf);
        case KAI_UINT32:
            snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.u32);
            return kai_str_dyn(buf);
        case KAI_UINT64:
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v->as.u64);
            return kai_str_dyn(buf);
        case KAI_INT128: {
            char i128buf[44];
            return kai_str_dyn(kai_i128_to_decimal(kai_i128_load(v), i128buf, sizeof(i128buf)));
        }
    }
    return kai_str("?");
}

static KAI_RC_NOINLINE KaiValue *kai_string_concat(KaiValue *a, KaiValue *b) {
    size_t la = (a && kai_is_ptr(a) && a->tag == KAI_STR) ? a->as.s.len : 0;
    size_t lb = (b && kai_is_ptr(b) && b->tag == KAI_STR) ? b->as.s.len : 0;
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = la + lb;
    v->as.s.bytes = (char *) kai_heap_malloc(la + lb + 1);
    if (la) memcpy(v->as.s.bytes, a->as.s.bytes, la);
    if (lb) memcpy(v->as.s.bytes + la, b->as.s.bytes, lb);
    v->as.s.bytes[la + lb] = '\0';
    return v;
}

/* Two-pass concat of every KAI_STR in the cons list: measure total
   length, allocate once, memcpy each piece in. Avoids the O(n²)
   accumulation that a naive fold of kai_string_concat produces, which
   dominates emit-heavy workloads like the self-hosting compiler. */
static KAI_RC_NOINLINE KaiValue *kai_string_concat_all_impl(KaiValue *xs) {
    size_t total = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (kai_is_ptr(s) && s->tag == KAI_STR) total += s->as.s.len;
    }
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = total;
    v->as.s.bytes = (char *) kai_heap_malloc(total + 1);
    size_t off = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR && s->as.s.len > 0) {
            memcpy(v->as.s.bytes + off, s->as.s.bytes, s->as.s.len);
            off += s->as.s.len;
        }
    }
    v->as.s.bytes[total] = '\0';
    return v;
}

/* Two-pass join: like concat_all but interleaves `sep` between pieces. */
static KAI_RC_NOINLINE KaiValue *kai_string_join_impl(KaiValue *xs, KaiValue *sep) {
    size_t slen = (kai_is_ptr(sep) && sep->tag == KAI_STR) ? sep->as.s.len : 0;
    size_t total = 0;
    int count = 0;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *s = p->as.cons.head;
        if (kai_is_ptr(s) && s->tag == KAI_STR) total += s->as.s.len;
        count++;
    }
    if (count > 1) total += slen * (size_t)(count - 1);
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = total;
    v->as.s.bytes = (char *) kai_heap_malloc(total + 1);
    size_t off = 0;
    int first = 1;
    for (KaiValue *p = xs; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        if (!first && slen > 0) {
            memcpy(v->as.s.bytes + off, sep->as.s.bytes, slen);
            off += slen;
        }
        first = 0;
        KaiValue *s = p->as.cons.head;
        if (s && s->tag == KAI_STR && s->as.s.len > 0) {
            memcpy(v->as.s.bytes + off, s->as.s.bytes, s->as.s.len);
            off += s->as.s.len;
        }
    }
    v->as.s.bytes[total] = '\0';
    return v;
}

/* ---------- core: IO ---------- */

/*
 * Calling convention for the core: borrow semantics. Arguments are
 * borrowed from the caller (no incref / no decref on them). Return
 * values are owned by the caller.
 */

/* Payload and trailing '\n' are separate stdio calls; each locks the
 * stream individually but the pair does not. flockfile around the pair
 * keeps a line atomic across scheduler threads. */
static KaiValue *kai_core_print(KaiValue *arg) {
    if (!arg) { fputc('\n', stdout); return kai_unit(); }
    KaiValue *s = (arg->tag == KAI_STR) ? NULL : kai_to_string(arg);
    KaiValue *src = s ? s : arg;
    flockfile(stdout);
    fwrite(src->as.s.bytes, 1, src->as.s.len, stdout);
    fputc('\n', stdout);
    funlockfile(stdout);
    if (s) kai_decref(s);
    kai_decref(arg);
    return kai_unit();
}

static KaiValue *kai_core_eprint(KaiValue *arg) {
    kai_flush_stdout();
    if (!arg) { fputc('\n', stderr); return kai_unit(); }
    KaiValue *s = (arg->tag == KAI_STR) ? NULL : kai_to_string(arg);
    KaiValue *src = s ? s : arg;
    flockfile(stderr);
    fwrite(src->as.s.bytes, 1, src->as.s.len, stderr);
    fputc('\n', stderr);
    funlockfile(stderr);
    if (s) kai_decref(s);
    kai_decref(arg);
    return kai_unit();
}

/*
 * Issue #447 (LSP framing): write a String to stdout verbatim with
 * no trailing newline, then flush. LSP JSON-RPC framing requires the
 * body to be exactly `Content-Length` bytes; `print` would inject an
 * extra '\n' that breaks strict clients. Non-String args fall through
 * `kai_to_string` for safety.
 */
static KaiValue *kai_core_write_stdout(KaiValue *arg) {
    if (!arg) { fflush(stdout); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stdout);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
        kai_decref(s);
    }
    fflush(stdout);
    kai_decref(arg);
    return kai_unit();
}

/* --debug stack trace (#500). `__kai_build_debug` is 0 in a default/release
 * binary and set to 1 by the native backend's debug marker global (emitted
 * only when DWARF is on). A weak definition here means a binary that never
 * defines the strong one still links. When it is 1, a panic resolves its
 * return-address backtrace to `<file>.kai:<line>` via the platform symboliser
 * (`atos` on macOS, `addr2line` on ELF) reading the DWARF this build emitted.
 * In a non-debug binary the flag is 0, so the panic stays the one-line form
 * and pays no symboliser cost. */
__attribute__((weak)) int __kai_build_debug = 0;

#if KAI_HOST_BACKTRACE
/* Best-effort absolute path of the running executable, into `buf`. Returns 1
 * on success. The symboliser needs it to read the binary's (or its .dSYM's)
 * DWARF. */
static int kai_self_exe_path(char *buf, size_t n) {
#if defined(__APPLE__)
    uint32_t sz = (uint32_t) n;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    return _NSGetExecutablePath(buf, &sz) == 0;
#else
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return 1;
#endif
}

/* Resolve and print the panic backtrace as kaikai source positions. Captures
 * the native return addresses, then shells out to the platform symboliser
 * once with all addresses (the DWARF line table maps each back to .kai:line).
 * Silent + harmless when the symboliser is absent or resolves nothing — the
 * caller already printed `panic: <msg>`. */
/* Single-quote-wrap `src` into `dst` for safe inclusion in a /bin/sh command
 * line, escaping any embedded `'` as `'\''` (close-quote, literal-quote,
 * re-open-quote). Without this a path containing a single quote would break
 * out of the quoting and the rest would be run by the shell — `exe` comes
 * from the executable's own path, but it is still attacker-influenceable
 * (a binary placed at a crafted path), so quote it properly. Truncates
 * safely if `dst` fills; returns 1 on success, 0 if it could not fit. */
static int kai_shell_squote(const char *src, char *dst, size_t n) {
    size_t j = 0;
    if (n < 3) return 0;
    dst[j++] = '\'';
    for (; *src; src++) {
        if (*src == '\'') {
            if (j + 4 >= n) return 0;
            dst[j++] = '\''; dst[j++] = '\\'; dst[j++] = '\''; dst[j++] = '\'';
        } else {
            if (j + 1 >= n) return 0;
            dst[j++] = *src;
        }
    }
    if (j + 2 > n) return 0;
    dst[j++] = '\''; dst[j] = '\0';
    return 1;
}

/* Return addresses of the live frames, newest first, into `out`.
 *
 * TRAP (macOS): `backtrace(3)` walks frame pointers but bounds the walk with
 * the *pthread* stack extent, so a panic raised on a fiber's mmap'd stack
 * faults inside libsystem's `__thread_stack_pcs` — the whole --debug panic
 * path dies with SIGSEGV instead of printing a trace. Walk the chain here
 * against the active fiber's own bounds; only the main fiber (stack_base
 * NULL) rides the thread stack, where `backtrace` is safe. */
static int kai_capture_frames(void **out, int max) {
#if defined(__APPLE__)
    KaiFiber *f = kai_current_fiber();
    if (!f || !f->stack_base) return backtrace(out, max);
    /* [guard page | usable stack]; frame pointers live in the usable half. */
    char *lo = (char *) f->stack_base + kai_page_size();
    char *hi = lo + f->stack_size;
    void **fp = (void **) __builtin_frame_address(0);
    int n = 0;
    while (n < max && (char *) fp >= lo && (char *) fp + 2 * sizeof(void *) <= hi) {
        void **next = (void **) fp[0];
        void  *ret  = fp[1];
        if (!ret) break;
        out[n++] = ret;
        if ((char *) next <= (char *) fp) break;   /* chain must climb */
        fp = next;
    }
    return n;
#else
    return backtrace(out, max);
#endif
}
#endif

KAI_RT_SHARED void kai_panic_backtrace(void) {
#if KAI_HOST_BACKTRACE
    void *frames[64];
    int n = kai_capture_frames(frames, 64);
    if (n <= 0) return;
    char exe[4096];
    if (!kai_self_exe_path(exe, sizeof exe)) return;
    char qexe[4112];   /* exe single-quoted for the shell (worst case grows it) */
    if (!kai_shell_squote(exe, qexe, sizeof qexe)) return;

    /* The frames are RUNTIME addresses (PIE/ASLR-slid). The symboliser reads
     * the binary at its STATIC link addresses, so the slide must be removed.
     * `dladdr` on this fn's own address yields `dli_fbase` — the load address
     * of the main image — which is exactly that slide. `atos -l <fbase>`
     * tells atos the load address (it subtracts internally); for `addr2line`
     * we subtract per-address and pass static offsets. */
    Dl_info info;
    uintptr_t base = 0;
    if (dladdr((void *) kai_panic_backtrace, &info) && info.dli_fbase)
        base = (uintptr_t) info.dli_fbase;

    char cmd[8192];
#if defined(__APPLE__)
    int off = snprintf(cmd, sizeof cmd, "atos -o %s -fullPath -l %p", qexe, (void *) base);
#else
    int off = snprintf(cmd, sizeof cmd, "addr2line -f -p -e %s", qexe);
#endif
    if (off <= 0 || off >= (int) sizeof cmd) return;
    /* Skip frame 0 (this fn) and frame 1 (kai_core_panic): start at the
     * panic's caller. atos gets the runtime address (it applies -l itself);
     * addr2line gets the static offset (runtime - base). */
    for (int i = 2; i < n && off < (int) sizeof cmd - 32; i++) {
#if defined(__APPLE__)
        off += snprintf(cmd + off, sizeof cmd - off, " %p", frames[i]);
#else
        off += snprintf(cmd + off, sizeof cmd - off, " %p",
                        (void *) ((uintptr_t) frames[i] - base));
#endif
    }

    fflush(stderr);
    FILE *p = popen(cmd, "r");
    if (!p) return;
    /* Print only the frames the symboliser resolved to a `.kai` source — the
     * runtime/libc frames (no `.kai`) are noise. Header is emitted lazily so
     * a build whose symboliser resolves nothing prints no empty header. */
    char line[1024];
    int printed = 0;
    while (fgets(line, sizeof line, p)) {
        if (!strstr(line, ".kai")) continue;
        if (!printed) { kai_eprintf("stack trace (kaikai source):\n"); printed = 1; }
        kai_eprintf("  %s", line);
    }
    pclose(p);
#else
    kai_eputs("kai: this host has no stack trace\n");
#endif
}

KAI_RT_SHARED KaiValue *kai_core_panic(KaiValue *msg) {
    KaiFiber *f = kai_current_fiber();
    if (f && f->cancel_pad_set) {
        static char buf[256];
        int n = 0;
        if (kai_is_ptr(msg) && msg->tag == KAI_STR) {
            n = (int) msg->as.s.len;
            if (n > (int) sizeof(buf) - 1) n = (int) sizeof(buf) - 1;
            memcpy(buf, msg->as.s.bytes, (size_t) n);
        }
        buf[n] = '\0';
        kai_decref(msg);
        kai_trap_abort(buf);
    }
    flockfile(stderr);
    kai_eprintf("panic: ");
    if (kai_is_ptr(msg) && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    funlockfile(stderr);
    if (__kai_build_debug) kai_panic_backtrace();
    kai_exit(1);
    return kai_unit();
}

static KaiValue *kai_core_exit(KaiValue *code) {
    int c = (kai_is_int(code)) ? (int) kai_intf(code) : 0;
    atomic_store(&kai_chosen_exit_plus1, (c & 0xff) + 1);
    kai_exit(c);
    return kai_unit();
}

/* ---------- core: conversions ---------- */

static KaiValue *kai_core_int_to_string(KaiValue *v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
    KaiValue *r = kai_str_dyn(buf);
    kai_decref(v);
    return r;
}

static KaiValue *kai_core_real_to_string(KaiValue *v) {
    char buf[64];
    kai_real_text(buf, sizeof(buf), v->as.r);
    KaiValue *r = kai_str_dyn(buf);
    kai_decref(v);
    return r;
}

static double kai_int_to_real_raw(int64_t n) { return (double) n; }

/* Truncating cast toward zero. Out-of-range, NaN and Inf collapse to 0. */
static int64_t kai_real_to_int_raw(double r) {
    if (r != r) return 0;
    if (r >= 9223372036854775808.0) return 0;
    if (r < -9.2233720368547758e18) return 0;
    return (int64_t) r;
}

static KaiValue *kai_core_int_to_real(KaiValue *v) {
    int64_t n = (kai_is_int(v)) ? kai_intf(v) : 0;
    KaiValue *r = kai_real((double) n);
    kai_decref(v);
    return r;
}

static KaiValue *kai_core_real_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_REAL) { if (v) kai_decref(v); return kai_int(0); }
    KaiValue *out = kai_int(kai_real_to_int_raw(v->as.r));
    kai_decref(v);
    return out;
}

/* ---------- Lane 4 (#473): Byte nominal scalar conversions + ops ----- */

/* `int_to_byte(n)` returns `Result[Byte, String]` — Err if n is outside
 * 0..255, Ok otherwise. The Result is encoded as a KAI_VARIANT (Ok /
 * Err) with one payload. */
static KaiValue *kai_core_int_to_byte(KaiValue *v) {
    if (!kai_is_int(v)) {
        if (v) kai_decref(v);
        KaiValue *err = kai_str("int_to_byte: not an Int");
        return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = err}});
    }
    int64_t n = kai_intf(v);
    kai_decref(v);
    if (n < 0 || n > 255) {
        char buf[64];
        snprintf(buf, sizeof(buf), "int_to_byte: %lld is out of 0..255", (long long) n);
        KaiValue *err = kai_str_dyn(buf);
        return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = err}});
    }
    KaiValue *ok_payload = kai_byte((uint8_t) n);
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = ok_payload}});
}

static KaiValue *kai_core_byte_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_int(0); }
    int64_t n = (int64_t) v->as.byte_val;
    kai_decref(v);
    return kai_int(n);
}

/* Wrapping arithmetic per uint8_t C semantics. Overflow is defined. */
static KaiValue *kai_core_byte_add(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av + bv));
}

static KaiValue *kai_core_byte_sub(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av - bv));
}

static KaiValue *kai_core_byte_eq(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av == bv);
}

static KaiValue *kai_core_byte_lt(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av < bv);
}

static KaiValue *kai_core_byte_to_string(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_str("0"); }
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
    kai_decref(v);
    return kai_str_dyn(buf);
}

/* Fixed-width integer Show prims (numeric lane A). Each consumes its
 * boxed argument (per the core consume discipline) and renders the
 * raw value via the same width-correct formatting as `kai_to_string`. */
static KaiValue *kai_core_int32_to_string(KaiValue *v) {
    int32_t n = (v && kai_is_ptr(v) && v->tag == KAI_INT32) ? v->as.i32 : 0;
    if (v) kai_decref(v);
    char buf[16]; snprintf(buf, sizeof(buf), "%d", (int) n);
    return kai_str_dyn(buf);
}
static KaiValue *kai_core_uint32_to_string(KaiValue *v) {
    uint32_t n = (v && kai_is_ptr(v) && v->tag == KAI_UINT32) ? v->as.u32 : 0;
    if (v) kai_decref(v);
    char buf[16]; snprintf(buf, sizeof(buf), "%u", (unsigned) n);
    return kai_str_dyn(buf);
}
static KaiValue *kai_core_uint64_to_string(KaiValue *v) {
    uint64_t n = (v && kai_is_ptr(v) && v->tag == KAI_UINT64) ? v->as.u64 : 0;
    if (v) kai_decref(v);
    char buf[24]; snprintf(buf, sizeof(buf), "%llu", (unsigned long long) n);
    return kai_str_dyn(buf);
}
static KaiValue *kai_core_int128_to_string(KaiValue *v) {
    __int128 n = (v && kai_is_ptr(v) && v->tag == KAI_INT128) ? kai_i128_load(v) : 0;
    if (v) kai_decref(v);
    char buf[44];
    return kai_str_dyn(kai_i128_to_decimal(n, buf, sizeof(buf)));
}

/* Fixed-width conversions. `int_to_*` truncates the 64-bit Int into the
 * width (C cast semantics, no range check); `*_to_int` widens back —
 * lossless for Int32/UInt32, a reinterpret for UInt64/Int128 values
 * beyond Int's range (documented; callers needing exact 128-bit values
 * keep them as Int128). */
static KaiValue *kai_core_int_to_int32(KaiValue *v)  { int64_t n = kai_intf(v); kai_decref(v); return kai_int32((int32_t) n); }
static KaiValue *kai_core_int_to_uint32(KaiValue *v) { int64_t n = kai_intf(v); kai_decref(v); return kai_uint32((uint32_t) n); }
static KaiValue *kai_core_int_to_uint64(KaiValue *v) { int64_t n = kai_intf(v); kai_decref(v); return kai_uint64((uint64_t) n); }
static KaiValue *kai_core_int_to_int128(KaiValue *v) { int64_t n = kai_intf(v); kai_decref(v); return kai_int128((__int128) n); }
static KaiValue *kai_core_int32_to_int(KaiValue *v)  { int32_t n  = (v && kai_is_ptr(v) && v->tag == KAI_INT32)  ? v->as.i32  : 0; kai_decref(v); return kai_int((int64_t) n); }
static KaiValue *kai_core_uint32_to_int(KaiValue *v) { uint32_t n = (v && kai_is_ptr(v) && v->tag == KAI_UINT32) ? v->as.u32  : 0; kai_decref(v); return kai_int((int64_t) n); }
static KaiValue *kai_core_uint64_to_int(KaiValue *v) { uint64_t n = (v && kai_is_ptr(v) && v->tag == KAI_UINT64) ? v->as.u64  : 0; kai_decref(v); return kai_int((int64_t) n); }
static KaiValue *kai_core_int128_to_int(KaiValue *v) { __int128 n = (v && kai_is_ptr(v) && v->tag == KAI_INT128) ? kai_i128_load(v) : 0; kai_decref(v); return kai_int((int64_t) n); }

/* ---------- core: math/real libm bindings (issue #343) ----------
 * IEEE-754 pass-through: NaN / Inf propagate per C99 <math.h>.
 * Inspect with kai_core_real_is_nan / kai_core_real_is_inf. */

#define KAI_LIBM_REAL1(name, fn)                                         \
    static KaiValue *kai_core_real_##name(KaiValue *x) {              \
        double r = (kai_is_ptr(x) && x->tag == KAI_REAL) ? fn(x->as.r) : 0.0;        \
        KaiValue *out = kai_real(r);                                     \
        if (x) kai_decref(x);                                            \
        return out;                                                      \
    }

#define KAI_LIBM_REAL2(name, fn)                                         \
    static KaiValue *kai_core_real_##name(KaiValue *a, KaiValue *b) { \
        double av = (a && kai_is_ptr(a) && a->tag == KAI_REAL) ? a->as.r : 0.0;           \
        double bv = (b && kai_is_ptr(b) && b->tag == KAI_REAL) ? b->as.r : 0.0;           \
        KaiValue *out = kai_real(fn(av, bv));                            \
        if (a) kai_decref(a);                                            \
        if (b) kai_decref(b);                                            \
        return out;                                                      \
    }

KAI_LIBM_REAL1(sqrt,  sqrt)
KAI_LIBM_REAL1(cbrt,  cbrt)
KAI_LIBM_REAL1(exp,   exp)
KAI_LIBM_REAL1(log,   log)
KAI_LIBM_REAL1(log2,  log2)
KAI_LIBM_REAL1(log10, log10)
KAI_LIBM_REAL1(sin,   sin)
KAI_LIBM_REAL1(cos,   cos)
KAI_LIBM_REAL1(tan,   tan)
KAI_LIBM_REAL1(asin,  asin)
KAI_LIBM_REAL1(acos,  acos)
KAI_LIBM_REAL1(atan,  atan)
KAI_LIBM_REAL1(sinh,  sinh)
KAI_LIBM_REAL1(cosh,  cosh)
KAI_LIBM_REAL1(tanh,  tanh)

KAI_LIBM_REAL2(pow,   pow)
KAI_LIBM_REAL2(atan2, atan2)
KAI_LIBM_REAL2(rem,   fmod)

#undef KAI_LIBM_REAL1
#undef KAI_LIBM_REAL2

/* signum: -1.0 / 0.0 / +1.0; NaN passes through as NaN. */
static KaiValue *kai_core_real_signum(KaiValue *x) {
    double r = (kai_is_ptr(x) && x->tag == KAI_REAL) ? x->as.r : 0.0;
    double s;
    if (r != r)        s = r;          /* NaN */
    else if (r > 0.0)  s = 1.0;
    else if (r < 0.0)  s = -1.0;
    else               s = 0.0;
    KaiValue *out = kai_real(s);
    if (x) kai_decref(x);
    return out;
}

static KaiValue *kai_core_real_is_nan(KaiValue *x) {
    int yes = 0;
    if (kai_is_ptr(x) && x->tag == KAI_REAL) { double r = x->as.r; yes = (r != r); }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
    return out;
}

static KaiValue *kai_core_real_is_inf(KaiValue *x) {
    int yes = 0;
    if (kai_is_ptr(x) && x->tag == KAI_REAL) {
        double r = x->as.r;
        yes = (r > 1.7976931348623157e308) || (r < -1.7976931348623157e308);
    }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
    return out;
}

/* ---------- core: strings ---------- */

static inline int64_t kai_core_string_length_raw(KaiValue *s) {
    return (kai_is_ptr(s) && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
}

static KaiValue *kai_core_string_length(KaiValue *s) {
    KaiValue *r = kai_int(kai_core_string_length_raw(s));
    if (s) kai_decref(s);
    return r;
}

static KaiValue *kai_core_string_concat(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_string_concat(a, b);
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return r;
}

static KaiValue *kai_core_string_concat_all(KaiValue *xs) {
    KaiValue *r = kai_string_concat_all_impl(xs);
    if (xs) kai_decref(xs);
    return r;
}

static KaiValue *kai_core_string_join(KaiValue *xs, KaiValue *sep) {
    KaiValue *r = kai_string_join_impl(xs, sep);
    if (xs) kai_decref(xs);
    if (sep) kai_decref(sep);
    return r;
}

