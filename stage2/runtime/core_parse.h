/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- core: parsing and string helpers ---------- */

/* m5.x flip Phase 3 closeout (Perceus Tier 2 audit, 2026-04-29):
 * the next 8 helpers consume their args linearly. Result computed
 * first, args decref'd before the constructor allocates. */
static KaiValue *kai_core_string_to_int(KaiValue *s) {
    int ok = 0;
    int64_t value = 0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0 && s->as.s.len < 64) {
        char buf[64];
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        /* strtoll saturates to LLONG_MIN/MAX on overflow and reports it
         * only through errno, which it never clears on success. */
        errno = 0;
        long long v = strtoll(buf, &end, 10);
        if (end && *end == '\0' && end != buf && errno != ERANGE) {
            value = (int64_t) v;
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (ok) {
        KaiValue *iv = kai_int(value);
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = iv}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

static KaiValue *kai_core_string_to_real(KaiValue *s) {
    int ok = 0;
    double value = 0.0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0) {
        char small[64];
        char *buf = s->as.s.len < sizeof small ? small : (char *) malloc(s->as.s.len + 1);
        if (!buf) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        /* strtod saturates to ±HUGE_VAL on overflow and reports it only
         * through errno. ERANGE also covers underflow, where the result
         * is a subnormal or zero — that is ordinary precision loss, not
         * a parse failure, so only the infinite case is rejected. */
        errno = 0;
        double v = strtod(buf, &end);
        if (end && *end == '\0' && end != buf
            && !(errno == ERANGE && (v >= HUGE_VAL || v <= -HUGE_VAL))) {
            value = v;
            ok = 1;
        }
        if (buf != small) free(buf);
    }
    if (s) kai_decref(s);
    if (ok) {
        KaiValue *rv = kai_real(value);
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = rv}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

static KaiValue *kai_core_char_at(KaiValue *s, KaiValue *i) {
    int ok = 0;
    uint32_t value = 0;
    if (s && s->tag == KAI_STR && i && kai_is_int(i)) {
        int64_t idx = kai_intf(i);
        if (idx >= 0 && (size_t) idx < s->as.s.len) {
            /* Byte-at semantics for now; multi-byte UTF-8 can return
             * surrogate-like codepoints once we need them. Stage 0
             * keeps it simple. */
            value = (uint32_t)(unsigned char) s->as.s.bytes[idx];
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (i) kai_decref(i);
    if (ok) {
        KaiValue *cv = kai_char(value);
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = cv}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

/* UTF-8 codepoint decode primitives (issue #744). `string.chars()`,
 * `char_count`, `char_indices` are written in kaikai over these two:
 * walk the byte buffer advancing `off += string_cp_len(s, off)` and
 * read the codepoint with `string_cp_at(s, off)`. Keeping the decode
 * in C (one lead-byte classification per call) avoids a kaikai byte
 * loop that would box an Int per byte. Lenient at the value level: a
 * malformed lead byte decodes to its raw byte value so a corrupt
 * buffer never traps here (the scalar-value invariant is enforced at
 * `int_to_char` / char-literal lexing, not on every decode). */
static int kai_utf8_seq_len(unsigned char b0) {
    if (b0 < 0x80)            return 1;
    if ((b0 & 0xE0) == 0xC0)  return 2;
    if ((b0 & 0xF0) == 0xE0)  return 3;
    if ((b0 & 0xF8) == 0xF0)  return 4;
    return 1;  /* malformed lead byte: consume one byte */
}

/* Byte-width (1..4) of the UTF-8 sequence starting at byte index `off`.
 * Returns 0 on out-of-range / wrong type so a kaikai walk terminates. */
static KaiValue *kai_core_string_cp_len(KaiValue *s, KaiValue *off) {
    int64_t w = 0;
    if (s && s->tag == KAI_STR && off && kai_is_int(off)) {
        int64_t i = kai_intf(off);
        if (i >= 0 && (size_t) i < s->as.s.len) {
            int seq = kai_utf8_seq_len((unsigned char) s->as.s.bytes[i]);
            size_t avail = s->as.s.len - (size_t) i;
            w = ((size_t) seq > avail) ? (int64_t) avail : (int64_t) seq;
        }
    }
    if (s)   kai_decref(s);
    if (off) kai_decref(off);
    return kai_int(w);
}

/* Decode the codepoint of the UTF-8 sequence starting at byte index
 * `off`. Returns -1 on out-of-range / wrong type. Continuation bytes
 * beyond the buffer are skipped (lenient, matches the clamp above). */
static KaiValue *kai_core_string_cp_at(KaiValue *s, KaiValue *off) {
    int64_t cp = -1;
    if (s && s->tag == KAI_STR && off && kai_is_int(off)) {
        int64_t i = kai_intf(off);
        if (i >= 0 && (size_t) i < s->as.s.len) {
            const unsigned char *b = (const unsigned char *) s->as.s.bytes;
            size_t len = s->as.s.len;
            unsigned char b0 = b[i];
            if (b0 < 0x80) {
                cp = b0;
            } else {
                uint32_t v;
                int extra;
                if      ((b0 & 0xE0) == 0xC0) { v = b0 & 0x1F; extra = 1; }
                else if ((b0 & 0xF0) == 0xE0) { v = b0 & 0x0F; extra = 2; }
                else if ((b0 & 0xF8) == 0xF0) { v = b0 & 0x07; extra = 3; }
                else                          { v = b0; extra = 0; }
                size_t j = (size_t) i + 1;
                while (extra-- > 0 && j < len) {
                    v = (v << 6) | (b[j] & 0x3F);
                    j++;
                }
                /* Malformed input can decode above U+10FFFF or into the
                 * surrogate range; map such non-scalar values to U+FFFD
                 * (replacement char) so a `chars()` walk never feeds a
                 * non-codepoint to `int_to_char` (which would panic).
                 * Well-formed UTF-8 never hits this. */
                if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) v = 0xFFFD;
                cp = (int64_t) v;
            }
        }
    }
    if (s)   kai_decref(s);
    if (off) kai_decref(off);
    return kai_int(cp);
}

/* Reverse `s` by Unicode codepoint in one pass and one allocation: walk
 * the source forward by UTF-8 sequence width and copy each FULL sequence,
 * byte order intact, into the destination filled from the tail. Byte order
 * within a codepoint is preserved; codepoint order is reversed. O(n), no
 * intermediate `[Char]`, no per-char re-encode. A truncated trailing
 * sequence (malformed input) is copied as-is via the clamped width. */
static KaiValue *kai_core_string_reverse(KaiValue *s) {
    if (!s || s->tag != KAI_STR) {
        if (s) kai_decref(s);
        return kai_str("");
    }
    size_t len = s->as.s.len;
    const unsigned char *src = (const unsigned char *) s->as.s.bytes;
    KaiValue *out = kai_alloc(KAI_STR);
    out->as.s.len = len;
    out->as.s.bytes = (char *) kai_heap_malloc(len + 1);
    out->as.s.bytes[len] = '\0';
    unsigned char *dst = (unsigned char *) out->as.s.bytes;
    size_t i = 0;
    size_t w = len;
    while (i < len) {
        int seq = kai_utf8_seq_len(src[i]);
        size_t avail = len - i;
        size_t step = ((size_t) seq > avail) ? avail : (size_t) seq;
        w -= step;
        memcpy(dst + w, src + i, step);
        i += step;
    }
    kai_decref(s);
    return out;
}

/* ASCII case fold in one allocation: bytes >= 0x80 are untouched, so a
 * multibyte sequence round-trips exact. `lo`..`hi` is the range shifted by
 * `delta`. Returns `s` itself when no byte changes. */
static KaiValue *kai_string_fold_ascii(KaiValue *s, unsigned char lo, unsigned char hi, int delta) {
    if (!s || s->tag != KAI_STR) {
        if (s) kai_decref(s);
        return kai_str("");
    }
    size_t len = s->as.s.len;
    const unsigned char *src = (const unsigned char *) s->as.s.bytes;
    size_t i = 0;
    while (i < len && !(src[i] >= lo && src[i] <= hi)) i++;
    if (i == len) return s;
    KaiValue *out = kai_alloc(KAI_STR);
    out->as.s.len = len;
    out->as.s.bytes = (char *) kai_heap_malloc(len + 1);
    unsigned char *dst = (unsigned char *) out->as.s.bytes;
    memcpy(dst, src, i);
    for (; i < len; i++) {
        unsigned char c = src[i];
        dst[i] = (c >= lo && c <= hi) ? (unsigned char) (c + delta) : c;
    }
    dst[len] = '\0';
    kai_decref(s);
    return out;
}

static KaiValue *kai_core_char_to_string(KaiValue *c) {
    uint32_t cp = (c && kai_is_ptr(c) && c->tag == KAI_CHAR) ? c->as.c : 0xFFFD;
    if (c) kai_decref(c);
    return kai_char_to_string(cp);
}

static KaiValue *kai_core_string_to_lower_ascii(KaiValue *s) { return kai_string_fold_ascii(s, 'A', 'Z', 32); }
static KaiValue *kai_core_string_to_upper_ascii(KaiValue *s) { return kai_string_fold_ascii(s, 'a', 'z', -32); }

/* First occurrence of `needle` in `hay`, or NULL; an empty needle matches at
 * the start. memchr on the first byte skips ahead a vector at a time. */
static inline const char *kai_mem_find(const char *hay, size_t hlen, const char *needle, size_t nlen) {
    if (nlen == 0) return hay;
    if (hlen < nlen) return NULL;
    const char *last = hay + (hlen - nlen);
    while (hay <= last) {
        hay = (const char *) memchr(hay, needle[0], (size_t) (last - hay) + 1);
        if (!hay) return NULL;
        if (memcmp(hay + 1, needle + 1, nlen - 1) == 0) return hay;
        hay++;
    }
    return NULL;
}

static KaiValue *kai_core_string_split(KaiValue *s, KaiValue *sep) {
    KaiValue *acc;
    if (!s || s->tag != KAI_STR) {
        acc = kai_nil();
    } else if (!sep || sep->tag != KAI_STR || sep->as.s.len == 0) {
        /* No separator → singleton list with own copy of `s`. */
        acc = kai_cons(kai_str_from_bytes(s->as.s.bytes, s->as.s.len), kai_nil());
    } else {
        const char *p = s->as.s.bytes;
        size_t slen = s->as.s.len;
        const char *sp = sep->as.s.bytes;
        size_t seplen = sep->as.s.len;
        /* Collect pieces into a temp array then fold right into a cons list. */
        size_t cap = 8, n = 0;
        struct { const char *b; size_t l; } *pieces = malloc(cap * sizeof(*pieces));
        const char *cur = p, *end = p + slen;
        for (;;) {
            const char *hit = kai_mem_find(cur, (size_t) (end - cur), sp, seplen);
            if (n == cap) { cap *= 2; pieces = realloc(pieces, cap * sizeof(*pieces)); }
            pieces[n].b = cur;
            pieces[n].l = (size_t) ((hit ? hit : end) - cur);
            n++;
            if (!hit) break;
            cur = hit + seplen;
        }
        acc = kai_nil();
        for (size_t k = n; k > 0;) {
            --k;
            acc = kai_cons(kai_str_from_bytes(pieces[k].b, pieces[k].l), acc);
        }
        free(pieces);
    }
    if (s) kai_decref(s);
    if (sep) kai_decref(sep);
    return acc;
}

static KaiValue *kai_core_string_slice(KaiValue *s, KaiValue *from, KaiValue *len) {
    KaiValue *r;
    if (!s || s->tag != KAI_STR) {
        r = kai_str("");
    } else {
        int64_t f = (kai_is_int(from)) ? kai_intf(from) : 0;
        int64_t l = (kai_is_int(len)) ? kai_intf(len)  : 0;
        if (f < 0) f = 0;
        if (l < 0) l = 0;
        if ((size_t) f > s->as.s.len) f = (int64_t) s->as.s.len;
        size_t avail = s->as.s.len - (size_t) f;
        size_t take  = ((size_t) l > avail) ? avail : (size_t) l;
        r = kai_str_from_bytes(s->as.s.bytes + f, take);
    }
    if (s)    kai_decref(s);
    if (from) kai_decref(from);
    if (len)  kai_decref(len);
    return r;
}

static KaiValue *kai_core_char_to_int(KaiValue *c) {
    int64_t value = (kai_is_ptr(c) && c->tag == KAI_CHAR) ? (int64_t) c->as.c : 0;
    if (c) kai_decref(c);
    return kai_int(value);
}

/* Issue #744: `int_to_char` carries the `Char` scalar-value invariant.
 * An argument outside the Unicode scalar-value range — negative, above
 * U+10FFFF, or in the surrogate range U+D800..U+DFFF — is not a
 * codepoint; constructing a `Char` from it is a programming error, so
 * we panic (an audited runtime escape per Tier 1 #1, the same class as
 * array out-of-bounds) rather than silently producing garbage. This
 * keeps every `Char` in a running program a valid scalar value while
 * leaving `int_to_char` total in its type. Byte values 0..255 (the
 * common "build a byte" idiom) are all valid scalar values and pass
 * unchanged. */
static KaiValue *kai_core_int_to_char(KaiValue *n) {
    int64_t cp = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (n) kai_decref(n);
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "int_to_char: %lld is not a Unicode scalar value "
                 "(0..0x10FFFF excluding surrogates)", (long long) cp);
        kai_core_panic(kai_str(msg));
    }
    return kai_char((uint32_t) cp);
}

/* Build a 1-byte String holding the low 8 bits of `n`. Unlike
 * `"#{int_to_char(n)}"` interpolation, which routes through
 * `kai_to_string(KAI_CHAR)` + `Show for Char` and escape-renders any
 * byte outside ASCII [32,126] (and truncates NUL via `strlen`), this
 * builtin constructs the string via `kai_str_from_bytes` so every
 * value 0..255 round-trips byte-exact. Cache layer (issue #592) uses
 * it for the KAB2 binary on-disk format. Documented limitation
 * sidebar in `stdlib/protocols.kai` (BinSerialize String/Real ASCII-
 * only) closes once Phase A/B serdes routes through this. */
static KaiValue *kai_core_int_to_byte_string(KaiValue *n) {
    int64_t v = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (n) kai_decref(n);
    unsigned char b = (unsigned char) (v & 0xff);
    return kai_str_from_bytes((const char *) &b, 1);
}

/* Encode an Int as its 4- / 8-byte little-endian block in one alloc.
 * The kaikai spellings emitted the same bytes one at a time and joined
 * them through a cons list, paying 5 and 10 allocations for a block
 * that fits in a register. Mirrors what the decoder side already does
 * with `string_byte_at_int`. */
static KaiValue *kai_core_int_to_le4(KaiValue *n) {
    int64_t v = (kai_is_int(n)) ? kai_intf(n) : 0;
    unsigned char b[4];
    int i;
    if (n) kai_decref(n);
    for (i = 0; i < 4; i++) b[i] = (unsigned char) ((uint64_t) v >> (i * 8));
    return kai_str_from_bytes((const char *) b, 4);
}

static KaiValue *kai_core_int_to_le8(KaiValue *n) {
    int64_t v = (kai_is_int(n)) ? kai_intf(n) : 0;
    unsigned char b[8];
    int i;
    if (n) kai_decref(n);
    for (i = 0; i < 8; i++) b[i] = (unsigned char) ((uint64_t) v >> (i * 8));
    return kai_str_from_bytes((const char *) b, 8);
}

/* Read one byte at index `i` of String `s`, returning it as an Int
 * 0..255. Returns -1 on out-of-bounds or wrong type. Faster than
 * `match char_at(s, i) { Some(c) -> char_to_int(c); None -> -1 }`
 * because it avoids the Option allocation + Char alloc + decref
 * chain (KAI_CHAR is cached for value < 128 but still hits a load
 * + tag check). The KAB2 cache decoder (#592) calls this ~1M times
 * per warm load; replacing Option-wrapped char_at cuts decoder wall
 * from ~0.26s to under 0.04s. */
static inline int64_t kai_core_string_byte_at_int_raw(KaiValue *s, int64_t idx) {
    int64_t v = -1;
    if (s && s->tag == KAI_STR && idx >= 0 && (size_t) idx < s->as.s.len) {
        v = (int64_t)(unsigned char) s->as.s.bytes[idx];
    }
    return v;
}

static KaiValue *kai_core_string_byte_at_int(KaiValue *s, KaiValue *i) {
    int64_t v = kai_core_string_byte_at_int_raw(s, (i && kai_is_int(i)) ? kai_intf(i) : -1);
    if (s) kai_decref(s);
    if (i) kai_decref(i);
    return kai_int(v);
}

/* ---------- core: Array[Byte] block primitives ----------
 *
 * Every Byte is an immortal cell of `kai_byte_cache`, so a Byte slot is
 * written by pointer store and a displaced one needs no decref. */

static int64_t kai_bytes_int(KaiValue *v) {
    int64_t n = kai_is_int(v) ? kai_intf(v) : 0;
    if (v) kai_decref(v);
    return n;
}

/* The slots `[off, off + n)` of `a`; traps when the range leaves the array. */
static KaiValue **kai_bytes_span(const char *op, KaiValue *a, int64_t off, int64_t n) {
    if (!kai_is_ptr(a) || a->tag != KAI_ARRAY) {
        kai_eprintf("kai: %s: not an array\n", op); kai_exit(1);
    }
    if (off < 0 || n < 0 || off > a->as.arr.len || n > a->as.arr.len - off) {
        /* Outlives this frame: a trap in a fiber is read after the unwind. */
        static char buf[128];
        snprintf(buf, sizeof buf, "%s: range %lld..%lld out of range (len=%lld)", op,
                 (long long) off, (long long) off + (long long) n, (long long) a->as.arr.len);
        kai_trap_abort(buf);
    }
    return a->as.arr.items + off;
}

static int64_t kai_bytes_width(const char *op, KaiValue *w) {
    int64_t n = kai_bytes_int(w);
    if (n < 0 || n > 8) {
        static char buf[96];
        snprintf(buf, sizeof buf, "%s: width %lld outside 0..8", op, (long long) n);
        kai_trap_abort(buf);
    }
    return n;
}

/* `dst[off..off+n) := src[soff..soff+n)`, overlap-safe; returns `dst`. */
static KaiValue *kai_core_bytes_blit(KaiValue *dst, KaiValue *off, KaiValue *src,
                                     KaiValue *soff, KaiValue *n) {
    int64_t o = kai_bytes_int(off), so = kai_bytes_int(soff), len = kai_bytes_int(n);
    KaiValue **d = kai_bytes_span("bytes_blit", dst, o, len);
    KaiValue **s = kai_bytes_span("bytes_blit", src, so, len);
    if (len > 0) memmove(d, s, (size_t) len * sizeof(KaiValue *));
    kai_decref(src);
    return dst;
}

/* The low `width` bytes of `v`, little-endian, at `dst[off]`; returns `dst`. */
static KaiValue *kai_core_bytes_put_le(KaiValue *dst, KaiValue *off, KaiValue *v, KaiValue *width) {
    int64_t o = kai_bytes_int(off), x = kai_bytes_int(v);
    int64_t w = kai_bytes_width("bytes_put_le", width);
    KaiValue **d = kai_bytes_span("bytes_put_le", dst, o, w);
    for (int64_t i = 0; i < w; ++i) d[i] = kai_byte((uint8_t) ((uint64_t) x >> (i * 8)));
    return dst;
}

/* `width` bytes at `src[off]`, little-endian, as an unsigned Int; width 8
 * wraps into the two's-complement Int. */
static KaiValue *kai_core_bytes_get_le(KaiValue *src, KaiValue *off, KaiValue *width) {
    int64_t o = kai_bytes_int(off);
    int64_t w = kai_bytes_width("bytes_get_le", width);
    KaiValue **s = kai_bytes_span("bytes_get_le", src, o, w);
    uint64_t acc = 0;
    for (int64_t i = w; i-- > 0;) acc = (acc << 8) | s[i]->as.byte_val;
    kai_decref(src);
    return kai_int((int64_t) acc);
}

/* The bytes of `s` at `dst[off]`; returns `dst`. */
static KaiValue *kai_core_bytes_put_string(KaiValue *dst, KaiValue *off, KaiValue *s) {
    int64_t o = kai_bytes_int(off);
    int64_t len = (kai_is_ptr(s) && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
    KaiValue **d = kai_bytes_span("bytes_put_string", dst, o, len);
    for (int64_t i = 0; i < len; ++i) d[i] = kai_byte((uint8_t) s->as.s.bytes[i]);
    if (s) kai_decref(s);
    return dst;
}

/* `src[off..off+n)` as a String, byte-exact. */
static KaiValue *kai_core_bytes_get_string(KaiValue *src, KaiValue *off, KaiValue *n) {
    int64_t o = kai_bytes_int(off), len = kai_bytes_int(n);
    KaiValue **s = kai_bytes_span("bytes_get_string", src, o, len);
    KaiValue *r = kai_alloc(KAI_STR);
    r->as.s.len = (size_t) len;
    r->as.s.bytes = (char *) kai_heap_malloc((size_t) len + 1);
    for (int64_t i = 0; i < len; ++i) r->as.s.bytes[i] = (char) s[i]->as.byte_val;
    r->as.s.bytes[len] = '\0';
    kai_decref(src);
    return r;
}

/* `string_hash(s)` — full-width 64-bit FNV-1a over the raw bytes, the
 * Hash protocol's String backend (issue #373). Distinct from the
 * interning hash `kai_str_intern_hash` above, which bucket-truncates
 * with `& (BUCKETS-1)`; here we return the *whole* 64-bit digest cast
 * to int64_t. That cast can wrap negative — that is correct and
 * intended. The future HashMap (#374) normalises to a bucket via
 * `((h % n) + n) % n` (or `h & (n-1)` for power-of-two n); negativity
 * is HashMap's contract to absorb, not Hash's to mask. Done in C, not
 * a kaikai byte loop, because the kaikai loop would box an Int per
 * byte (one alloc + decref per character) — catastrophic for the hot
 * path a HashMap exercises. */
static KaiValue *kai_core_string_hash(KaiValue *s) {
    uint64_t h = kai_fnv1a64("", 0);
    if (kai_is_ptr(s) && s->tag == KAI_STR) h = kai_fnv1a64(s->as.s.bytes, s->as.s.len);
    if (s) kai_decref(s);
    return kai_int((int64_t) h);
}

/* `real_bits(r)` — reinterpret a double's IEEE-754 bit pattern as an
 * Int, the Hash protocol's Real backend (issue #373). A bit-cast
 * rather than `int_of_real` truncation because truncation collapses
 * the fraction (1.5 and 1.9 would hash identically); the bit pattern
 * separates every distinct double. Note +0.0 and -0.0 have different
 * bit patterns (and NaN payloads vary) — acceptable for a hash, since
 * Hash↔Eq consistency on Real is the caller's concern, not ours. */
static KaiValue *kai_core_real_bits(KaiValue *v) {
    uint64_t bits = 0;
    if (kai_is_ptr(v) && v->tag == KAI_REAL) {
        double r = v->as.r;
        memcpy(&bits, &r, sizeof(bits));
    }
    if (v) kai_decref(v);
    return kai_int((int64_t) bits);
}

/* Byte offset of the first `sub` in `s`, or -1; borrows both. */
static int64_t kai_str_find(KaiValue *s, KaiValue *sub) {
    if (!s || s->tag != KAI_STR || !sub || sub->tag != KAI_STR) return -1;
    if (sub->as.s.len == 0) return 0;
    const char *hit = kai_mem_find(s->as.s.bytes, s->as.s.len, sub->as.s.bytes, sub->as.s.len);
    return hit ? (int64_t) (hit - s->as.s.bytes) : -1;
}

static KaiValue *kai_core_string_contains(KaiValue *s, KaiValue *sub) {
    int yes = kai_str_find(s, sub) >= 0;
    if (s)   kai_decref(s);
    if (sub) kai_decref(sub);
    return kai_bool(yes);
}

static KaiValue *kai_core_string_find(KaiValue *s, KaiValue *sub) {
    int64_t at = kai_str_find(s, sub);
    if (s)   kai_decref(s);
    if (sub) kai_decref(sub);
    return kai_int(at);
}

/* Digit value of an ASCII byte in base 36; 36 for a byte that is no digit. */
static int kai_radix_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 36;
}

/* Accumulates on the negative side, which holds INT64_MIN. */
static KaiValue *kai_core_to_int_radix(KaiValue *s, KaiValue *r) {
    int64_t radix = kai_intf(r);
    int ok = 0;
    int64_t acc = 0;
    if (s && s->tag == KAI_STR && radix >= 2 && radix <= 36) {
        const unsigned char *p = (const unsigned char *) s->as.s.bytes;
        size_t n = (size_t) s->as.s.len, i = 0;
        int neg = n > 0 && p[0] == '-';
        if (n > 0 && (p[0] == '-' || p[0] == '+')) i = 1;
        ok = i < n;
        for (; ok && i < n; i++) {
            int d = kai_radix_digit(p[i]);
            if (d >= radix || acc < (INT64_MIN + d) / radix) ok = 0;
            else acc = acc * radix - d;
        }
        if (ok && !neg) {
            if (acc == INT64_MIN) ok = 0;
            else acc = -acc;
        }
    }
    if (s) kai_decref(s);
    kai_decref(r);
    if (ok) {
        KaiValue *iv = kai_int(acc);
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = iv}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

/* Divides on the non-positive side so INT64_MIN needs no negation. */
static KaiValue *kai_core_int_to_string_radix(KaiValue *v, KaiValue *r) {
    int64_t n = kai_intf(v), radix = kai_intf(r);
    kai_decref(v);
    kai_decref(r);
    if (radix < 2 || radix > 36) return kai_variant_u(1, "None", 0, 0, NULL);
    char buf[72];
    char *end = buf + sizeof buf, *p = end;
    int64_t m = n < 0 ? n : -n;
    do {
        *--p = "0123456789abcdefghijklmnopqrstuvwxyz"[-(m % radix)];
        m /= radix;
    } while (m != 0);
    if (n < 0) *--p = '-';
    KaiValue *sv = kai_str_from_bytes(p, (size_t) (end - p));
    return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = sv}});
}

/* ---------- core thunks for first-class function refs ---------- */

static KaiValue *_kai_core_print_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_core_print(a[0]); }
static KaiValue *_kai_core_eprint_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_core_eprint(a[0]); }
static KaiValue *_kai_core_write_stdout_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_write_stdout(a[0]); }
static KaiValue *_kai_core_panic_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_core_panic(a[0]); }
static KaiValue *_kai_core_exit_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_core_exit(a[0]); }
static KaiValue *_kai_core_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_int_to_string(a[0]); }
static KaiValue *_kai_core_real_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_real_to_string(a[0]); }
static KaiValue *_kai_core_int_to_real_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_int_to_real(a[0]); }
static KaiValue *_kai_core_real_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_real_to_int(a[0]); }
static KaiValue *_kai_core_real_sqrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_sqrt(a[0]); }
static KaiValue *_kai_core_real_cbrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_cbrt(a[0]); }
static KaiValue *_kai_core_real_exp_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_exp(a[0]); }
static KaiValue *_kai_core_real_log_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_log(a[0]); }
static KaiValue *_kai_core_real_log2_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_log2(a[0]); }
static KaiValue *_kai_core_real_log10_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_real_log10(a[0]); }
static KaiValue *_kai_core_real_sin_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_sin(a[0]); }
static KaiValue *_kai_core_real_cos_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_cos(a[0]); }
static KaiValue *_kai_core_real_tan_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_tan(a[0]); }
static KaiValue *_kai_core_real_asin_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_asin(a[0]); }
static KaiValue *_kai_core_real_acos_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_acos(a[0]); }
static KaiValue *_kai_core_real_atan_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_atan(a[0]); }
static KaiValue *_kai_core_real_sinh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_sinh(a[0]); }
static KaiValue *_kai_core_real_cosh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_cosh(a[0]); }
static KaiValue *_kai_core_real_tanh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_real_tanh(a[0]); }
static KaiValue *_kai_core_real_signum_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_real_signum(a[0]); }
static KaiValue *_kai_core_real_is_nan_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_real_is_nan(a[0]); }
static KaiValue *_kai_core_real_is_inf_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_real_is_inf(a[0]); }
static KaiValue *_kai_core_real_pow_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_pow(a[0], a[1]); }
static KaiValue *_kai_core_real_atan2_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_real_atan2(a[0], a[1]); }
static KaiValue *_kai_core_real_rem_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_real_rem(a[0], a[1]); }
static KaiValue *_kai_core_string_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_string_length(a[0]); }
static KaiValue *_kai_core_string_concat_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_string_concat(a[0], a[1]); }
static KaiValue *_kai_core_string_concat_all_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_concat_all(a[0]); }
static KaiValue *_kai_core_string_join_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_string_join(a[0], a[1]); }
static KaiValue *_kai_core_array_make_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_array_make(a[0], a[1]); }
static KaiValue *_kai_core_array_empty_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) a; (void) n; return kai_core_array_empty(); }
static KaiValue *_kai_core_array_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_array_length(a[0]); }
static KaiValue *_kai_core_array_get_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_array_get(a[0], a[1]); }
static KaiValue *_kai_core_array_take_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_array_take(a[0], a[1]); }
static KaiValue *_kai_core_array_set_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_array_set(a[0], a[1], a[2]); }
static KaiValue *_kai_core_array_grow_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_array_grow(a[0], a[1], a[2]); }
static KaiValue *_kai_core_ref_make_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_ref_make(a[0]); }
static KaiValue *_kai_core_ref_get_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_ref_get(a[0]); }
static KaiValue *_kai_core_ref_set_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_ref_set(a[0], a[1]); }
static KaiValue *_kai_core_list_length_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_list_length(a[0]); }
static KaiValue *_kai_core_list_append_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_list_append(a[0], a[1]); }
static KaiValue *_kai_core_list_reverse_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_list_reverse(a[0]); }
static KaiValue *_kai_core_map_thunk(KaiValue *s, KaiValue **a, int n)            { (void) s; (void) n; return kai_core_map(a[0], a[1]); }
static KaiValue *_kai_core_flat_map_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_flat_map(a[0], a[1]); }
static KaiValue *_kai_core_filter_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_core_filter(a[0], a[1]); }
static KaiValue *_kai_core_reduce_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_core_reduce(a[0], a[1], a[2]); }
static KaiValue *_kai_core___deep_apply1_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core___deep_apply1(a[0], a[1]); }
static KaiValue *_kai_core___deep_apply2_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core___deep_apply2(a[0], a[1], a[2]); }
static KaiValue *_kai_core_each_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_core_each(a[0], a[1]); }
static KaiValue *_kai_core_args_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) a; (void) n; return kai_core_args(); }
static KaiValue *_kai_core_program_name_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) a; (void) n; return kai_core_program_name(); }
static KaiValue *_kai_core_stdlib_path_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) a; (void) n; return kai_core_stdlib_path(); }
static KaiValue *_kai_core_abspath_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_core_abspath(a[0]); }
static KaiValue *_kai_core_read_file_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_core_read_file(a[0]); }
static KaiValue *_kai_core_write_file_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_write_file(a[0], a[1]); }
static KaiValue *_kai_core_file_append_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_file_append(a[0], a[1]); }
static KaiValue *_kai_core_file_exists_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_file_exists(a[0]); }
static KaiValue *_kai_core_file_delete_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_file_delete(a[0]); }
static KaiValue *_kai_core_file_rename_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_file_rename(a[0], a[1]); }
static KaiValue *_kai_core_file_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_file_read_bytes(a[0]); }
static KaiValue *_kai_core_file_write_bytes_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_file_write_bytes(a[0], a[1]); }
static KaiValue *_kai_core_dir_list_dir_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_dir_list_dir(a[0]); }
static KaiValue *_kai_core_dir_create_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_dir_create_dir(a[0]); }
static KaiValue *_kai_core_dir_remove_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_dir_remove_dir(a[0]); }
static KaiValue *_kai_core_dir_walk_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_core_dir_walk(a[0]); }
static KaiValue *_kai_core_file_metadata_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_file_metadata(a[0]); }
static KaiValue *_kai_core_dir_current_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) a; (void) n; return kai_core_dir_current_dir(); }
static KaiValue *_kai_core_dir_temp_dir_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) a; (void) n; return kai_core_dir_temp_dir(); }
static KaiValue *_kai_core_read_line_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) a; (void) n; return kai_core_read_line(); }
static KaiValue *_kai_core_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_core_read_bytes(a[0]); }
static KaiValue *_kai_core_string_to_int_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_string_to_int(a[0]); }
static KaiValue *_kai_core_string_to_real_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_to_real(a[0]); }
static KaiValue *_kai_core_char_at_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_core_char_at(a[0], a[1]); }
static KaiValue *_kai_core_string_split_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_string_split(a[0], a[1]); }
static KaiValue *_kai_core_string_contains_thunk(KaiValue *s, KaiValue **a, int n){ (void) s; (void) n; return kai_core_string_contains(a[0], a[1]); }
static KaiValue *_kai_core_string_slice_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_string_slice(a[0], a[1], a[2]); }
static KaiValue *_kai_core_char_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_char_to_int(a[0]); }
static KaiValue *_kai_core_int_to_char_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_core_int_to_char(a[0]); }
static KaiValue *_kai_core_int_to_byte_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_int_to_byte_string(a[0]); }
static KaiValue *_kai_core_string_byte_at_int_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_byte_at_int(a[0], a[1]); }
static KaiValue *_kai_core_string_cp_at_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_core_string_cp_at(a[0], a[1]); }
static KaiValue *_kai_core_string_cp_len_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_cp_len(a[0], a[1]); }
static KaiValue *_kai_core_string_reverse_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_reverse(a[0]); }
static KaiValue *_kai_core_char_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_char_to_string(a[0]); }
static KaiValue *_kai_core_string_to_lower_ascii_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_to_lower_ascii(a[0]); }
static KaiValue *_kai_core_string_to_upper_ascii_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_to_upper_ascii(a[0]); }
static KaiValue *_kai_core_string_hash_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_string_hash(a[0]); }
static KaiValue *_kai_core_real_bits_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_real_bits(a[0]); }
static KaiValue *_kai_core_mailbox_alloc_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) a; (void) n; return kai_core_mailbox_alloc(); }
static KaiValue *_kai_core_mailbox_alloc_bounded_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_mailbox_alloc_bounded(a[0], a[1]); }
static KaiValue *_kai_core_mailbox_send_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_mailbox_send(a[0], a[1]); }
static KaiValue *_kai_core_mailbox_recv_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_mailbox_recv(a[0]); }
static KaiValue *_kai_core_mailbox_recv_timeout_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_core_mailbox_recv_timeout(a[0], a[1]); }
static KaiValue *_kai_core_mailbox_free_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_core_mailbox_free(a[0]); }

/* The rest of the registry, by arity. Every `core_table()` entry
 * (compiler/core_prims.kai) needs a thunk here: a builtin named as a
 * value becomes a closure over it, and a missing one is a compile error
 * in the generated C. */
#define KAI_CORE_THUNK0(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) a; (void) n; return kai_core_##nm(); }
#define KAI_CORE_THUNK1(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) n; return kai_core_##nm(a[0]); }
#define KAI_CORE_THUNK2(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) n; return kai_core_##nm(a[0], a[1]); }
#define KAI_CORE_THUNK3(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) n; return kai_core_##nm(a[0], a[1], a[2]); }
#define KAI_CORE_THUNK4(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) n; return kai_core_##nm(a[0], a[1], a[2], a[3]); }
#define KAI_CORE_THUNK5(nm) \
    static KaiValue *_kai_core_##nm##_thunk(KaiValue *s, KaiValue **a, int n) \
    { (void) s; (void) n; return kai_core_##nm(a[0], a[1], a[2], a[3], a[4]); }

KAI_CORE_THUNK2(to_int_radix)
KAI_CORE_THUNK2(int_to_string_radix)
KAI_CORE_THUNK2(string_find)
KAI_CORE_THUNK1(int_to_le4)
KAI_CORE_THUNK1(int_to_le8)
KAI_CORE_THUNK5(bytes_blit)
KAI_CORE_THUNK4(bytes_put_le)
KAI_CORE_THUNK3(bytes_get_le)
KAI_CORE_THUNK3(bytes_put_string)
KAI_CORE_THUNK3(bytes_get_string)
KAI_CORE_THUNK1(int_to_byte)
KAI_CORE_THUNK1(byte_to_int)
KAI_CORE_THUNK2(byte_add)
KAI_CORE_THUNK2(byte_sub)
KAI_CORE_THUNK2(byte_eq)
KAI_CORE_THUNK2(byte_lt)
KAI_CORE_THUNK1(byte_to_string)
KAI_CORE_THUNK1(int32_to_string)
KAI_CORE_THUNK1(uint32_to_string)
KAI_CORE_THUNK1(uint64_to_string)
KAI_CORE_THUNK1(int128_to_string)
KAI_CORE_THUNK1(int_to_int32)
KAI_CORE_THUNK1(int_to_uint32)
KAI_CORE_THUNK1(int_to_uint64)
KAI_CORE_THUNK1(int_to_int128)
KAI_CORE_THUNK1(int32_to_int)
KAI_CORE_THUNK1(uint32_to_int)
KAI_CORE_THUNK1(uint64_to_int)
KAI_CORE_THUNK1(int128_to_int)
KAI_CORE_THUNK1(array_length_borrow)
KAI_CORE_THUNK2(array_same)
KAI_CORE_THUNK2(array_get_borrow)
KAI_CORE_THUNK2(vec_make)
KAI_CORE_THUNK0(vec_empty)
KAI_CORE_THUNK1(vec_length)
KAI_CORE_THUNK2(vec_get)
KAI_CORE_THUNK3(vec_set)
KAI_CORE_THUNK2(vec_push)
KAI_CORE_THUNK1(vec_length_borrow)
KAI_CORE_THUNK2(vec_get_borrow)
KAI_CORE_THUNK3(vec_slice)
KAI_CORE_THUNK2(vec_tail_from)
KAI_CORE_THUNK1(vec_reserve)
KAI_CORE_THUNK1(vec_from_list)
KAI_CORE_THUNK0(mailbox_alloc_unowned)
KAI_CORE_THUNK2(mailbox_alloc_bounded_unowned)
KAI_CORE_THUNK2(mailbox_assign_owner)
KAI_CORE_THUNK2(spawn_actor_fiber)

