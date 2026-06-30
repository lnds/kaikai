/*
 * kaikai-minimal runtime for stage 0.
 *
 * Header-only. Generated C files #include this and compile together in
 * one translation unit, so static storage and linkage are fine.
 *
 * All values are heap-allocated KaiValues with a reference-count header
 * and a discriminating tag. Uniform boxing per docs/stage0-design.md;
 * primitives (Int, Real, Bool, Char, Unit) are wrapped in the same
 * struct so the generated code does not branch on shape.
 *
 * Calling convention for functions emitted by stage 0:
 *   static KaiValue *kai_<name>(KaiValue *arg0, KaiValue *arg1, ...);
 * Arguments are handed to the callee as owned references (callee must
 * decref them or keep them alive as part of the returned value). The
 * return value is owned by the caller.
 *
 * Closures and higher-order prelude helpers (map/filter/reduce/each)
 * go through a dynamic dispatch path `kai_apply(closure, argc, argv)`
 * to keep the generated code uniform.
 */

/* m8.x cooperative scheduler substrate: ucontext.
 *
 * The _XOPEN_SOURCE feature-test macro must be defined BEFORE ANY
 * system header is included, otherwise sys/types.h (transitively
 * pulled in by stdio.h, time.h, etc.) freezes the legacy POSIX
 * ucontext_t layout (~56 bytes) instead of the full XSI shape
 * (~880 bytes on darwin arm64). swapcontext then writes 880 bytes
 * into a 56-byte buffer, silently corrupting whatever sits next to
 * the embedded ucontext_t — exactly what bit Phase 2 once
 * (kai_main_fiber.evidence_top got clobbered to a saved register
 * value because the static evidence nodes were laid out adjacent).
 * Spec: docs/fibers-impl.md §*macOS deprecation handling*. */
#define _XOPEN_SOURCE 600
/* mmap(MAP_ANON) is a BSD extension hidden by strict _XOPEN_SOURCE.
 * Re-expose it: _DARWIN_C_SOURCE on macOS, _DEFAULT_SOURCE on glibc.
 * The fiber stack allocator (m8.x guard pages) needs anonymous mmap. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif
#if defined(__linux__)
#  define _DEFAULT_SOURCE 1
#endif

#ifndef KAI_RUNTIME_H
#define KAI_RUNTIME_H

#include <dirent.h>
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* net-tcp-v1 — sockets API for the NetTcp default handler.
 * POSIX everywhere we ship: macOS, Linux, *BSD. The handler is
 * blocking-only: the m8.x cooperative scheduler (landed v0.4.0)
 * suspends fibers on mailbox / await / yield, but we have no
 * readiness reactor yet, so socket reads/writes park the OS thread
 * rather than the fiber. kqueue / epoll integration is a Tier 2
 * follow-up tracked in docs/fibers-honesty-targets.md §Reactor. */
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Issue #611 — Phase R1 reactor support. `poll()` is the wait
 * primitive (POSIX everywhere we ship), `pthread` powers the file
 * I/O worker pool, and `fcntl` toggles O_NONBLOCK on the self-pipes
 * so the SIGCHLD handler and worker threads never block on write.
 * Sockets stay on the blocking path until R2. */
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>

/* Stack buffer size for filesystem path operations (realpath, dirname,
 * join, etc.). Replaces the bare 4096 literal that was copy-pasted across
 * ~14 path sites. Floors at 4096 to preserve the historical buffer size
 * on every platform: darwin defines PATH_MAX as 1024 and Linux as 4096,
 * so a plain `PATH_MAX` would have *shrunk* the macOS buffer and started
 * truncating paths in [1024, 4096). The max() keeps the old behaviour
 * everywhere and only grows on systems with a larger PATH_MAX. POSIX lets
 * PATH_MAX be undefined when the max is unbounded (GNU/Hurd); the floor
 * covers that case too. */
#if defined(PATH_MAX) && PATH_MAX > 4096
#  define KAI_PATH_BUF PATH_MAX
#else
#  define KAI_PATH_BUF 4096
#endif

/* Stack buffer for environment-variable *names* (get_var/set_var/unset_var).
 * 1024 was copy-pasted across the three env entry points; named here so the
 * three stay in lock-step. POSIX does not bound env name length, but 1024 is
 * far past any real variable name; over-long names are truncated, matching
 * the prior behaviour. */
#define KAI_ENV_NAME_BUF 1024

/* Stack buffer for network host strings passed to getaddrinfo (connect and
 * listen sites). NI_MAXHOST is 1025 on most platforms; 256 is the historical
 * pragmatic bound shared by both call sites — kept as-is, just named. */
#define KAI_NET_HOST_BUF 256

/* Initial heap capacity for the grow-by-doubling stdin read buffers
 * (read_line and the stdin event loop). The buffer doubles via realloc, so
 * this is only the starting point, not a ceiling; 128 covers a typical line
 * without a first reallocation. */
#define KAI_READ_BUF_INIT 128

/* listen(2) backlog for server sockets. Linux caps the effective value at
 * net.core.somaxconn regardless; 128 is the conservative v1 bound shared by
 * every listen site. */
#define KAI_LISTEN_BACKLOG 128

/* Default permission bits for directories created by dir_create (rwxr-xr-x,
 * umask-masked by the kernel). Named to document intent; value unchanged. */
#define KAI_DIR_MODE 0755

/* Apple deprecated ucontext in macOS 10.6 (POSIX-2008 obsoletion).
 * The functions still work; the deprecation attribute on the
 * prototypes triggers -Wdeprecated-declarations, which the local
 * pragma silences. Same trick libco / libtask / Boost.Context use.
 * The same envelope wraps every swap/get/makecontext call site. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <ucontext.h>
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

/* Not every program uses every prelude function; silence the
   unused-function warnings that would otherwise pile up in `cc` output
   when stage 0 links only the parts a given program needs. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* ---------- types ---------- */

typedef enum {
    KAI_UNIT,
    KAI_BOOL,
    KAI_INT,
    KAI_REAL,
    KAI_CHAR,
    KAI_STR,
    KAI_NIL,
    KAI_CONS,
    KAI_RECORD,
    KAI_VARIANT,
    KAI_CLOSURE,
    KAI_ARRAY,
    KAI_REF,        /* Ref[T]: single-cell mutable reference (Mutable effect).
                     * Distinct from KAI_ARRAY — a Ref is exactly one slot, with
                     * no length / capacity / indexing. The earlier hack backed
                     * Ref with a length-1 KAI_ARRAY (issue #257); this is the
                     * "clean fix" the #257 retro flagged. ML/OCaml `ref`,
                     * Haskell `IORef` lineage. */
    KAI_FIBER,      /* m8 #3: Spawn / Fiber[T] handle (opaque) */
    KAI_PID,        /* m8 #7: Actor[Msg] / Pid[Msg] handle (opaque) */
    KAI_BYTE,         /* Lane 4 (#473): unsigned 8-bit integer, nominal */
    KAI_FOREIGN       /* FFI v2 (#417): opaque C handle (extern "C" opaque T).
                       * Parks a raw `void *` the kaikai side threads through
                       * but never inspects. RC manages the box; the parked
                       * pointer is NEVER freed (no Drop integration — the
                       * driver owns that lifetime). Identity-compared. */
} KaiTag;

/* Head-type tags — single-dispatch protocol dispatch key.
 * See docs/variant-tags.md "Head-type tags — protocol dispatch key".
 *
 * Tags 0..15 are reserved for primitives + structural; stdlib sums
 * pin 16..19; user-declared nominal types start at 20. */
#define KAI_HEAD_ANON         0
#define KAI_HEAD_UNIT         1
#define KAI_HEAD_BOOL         2
#define KAI_HEAD_INT          3
#define KAI_HEAD_REAL         4
#define KAI_HEAD_CHAR         5
#define KAI_HEAD_STRING       6
#define KAI_HEAD_LIST         7
#define KAI_HEAD_ARRAY        8
#define KAI_HEAD_BYTE         9
#define KAI_HEAD_CLOSURE     10
#define KAI_HEAD_FIBER       11
#define KAI_HEAD_PID         12
#define KAI_HEAD_BYTES       13
#define KAI_HEAD_OPTION      16
#define KAI_HEAD_RESULT      17
#define KAI_HEAD_SIGNAL      18
#define KAI_HEAD_PROCESS_EXIT 19
#define KAI_USER_HEAD_TAG_BASE 20

/* Protocol IDs — first 12 reserved for stdlib protocols in
 * stdlib/protocols.kai declaration order. User protocols start at 12.
 * See docs/variant-tags.md "Protocol IDs". */
#define KAI_PROTO_SHOW          0
#define KAI_PROTO_EQ            1
#define KAI_PROTO_ORD           2
#define KAI_PROTO_HASH          3
#define KAI_PROTO_SERIALIZE     4
#define KAI_PROTO_BIN_SERIALIZE 5
#define KAI_PROTO_DEFAULT       6
#define KAI_PROTO_ADD           7
#define KAI_PROTO_SUB           8
#define KAI_PROTO_MUL           9
#define KAI_PROTO_DIV          10
#define KAI_PROTO_REM          11
#define KAI_PROTO_NUMERIC      12
#define KAI_USER_PROTO_ID_BASE 13

/* Operation index within a protocol (declaration order in
 * stdlib/protocols.kai). The impl table keys on (proto_id, op_id, head_tag):
 * a multi-op protocol (Ord, Numeric) registers one impl per op for the same
 * (proto_id, head_tag), so the op_id disambiguates which impl a dispatcher
 * or a bare-op-as-value lookup resolves. Single-op protocols use op 0. */
#define KAI_OP_ORD_CMP          0
#define KAI_OP_ORD_MIN          1
#define KAI_OP_ORD_MAX          2
#define KAI_OP_EQ_EQ            0

/* Variant-tag -> head-type-tag map. Set once at startup by codegen-
 * emitted main via kai_register_variant_heads(table, len). Until set,
 * the bootstrap table covers the 11 reserved builtin variants
 * (Some/None -> Option, Ok/Err -> Result, Sig* -> Signal,
 * Exited/Signaled -> ProcessExit). */
static const int32_t kai_variant_to_head_bootstrap[11] = {
    /* 0  */ KAI_HEAD_OPTION,        /* Some  */
    /* 1  */ KAI_HEAD_OPTION,        /* None  */
    /* 2  */ KAI_HEAD_RESULT,        /* Ok    */
    /* 3  */ KAI_HEAD_RESULT,        /* Err   */
    /* 4  */ KAI_HEAD_SIGNAL,        /* SigInt  */
    /* 5  */ KAI_HEAD_SIGNAL,        /* SigTerm */
    /* 6  */ KAI_HEAD_SIGNAL,        /* SigHup  */
    /* 7  */ KAI_HEAD_SIGNAL,        /* SigUsr1 */
    /* 8  */ KAI_HEAD_SIGNAL,        /* SigUsr2 */
    /* 9  */ KAI_HEAD_PROCESS_EXIT,  /* Exited   */
    /* 10 */ KAI_HEAD_PROCESS_EXIT,  /* Signaled */
};

static const int32_t *kai_variant_to_head     = kai_variant_to_head_bootstrap;
static int32_t        kai_variant_to_head_len = 11;

static inline void kai_register_variant_heads(const int32_t *tbl, int32_t len) {
    kai_variant_to_head     = tbl;
    kai_variant_to_head_len = len;
}

/* Impl-table entry — emitted by codegen as a static const array, then
 * loaded into the runtime hashmap at startup by kai_register_impls. */
typedef struct {
    int32_t proto_id;
    int32_t op_id;
    int32_t head_tag;
    void   *fn;
} KaiImplEntry;

/* Open-addressing hashmap, linear probing. Capacity is a power of 2.
 * Empty slot marked by fn == NULL (no impl ever has NULL function).
 * Key is (proto_id, op_id, head_tag): a multi-op protocol registers one
 * impl per op for the same (proto_id, head_tag), so op_id disambiguates. */
typedef struct {
    int32_t proto_id;
    int32_t op_id;
    int32_t head_tag;
    void   *fn;
} KaiImplSlot;

static KaiImplSlot *kai_impl_table  = NULL;
static int32_t      kai_impl_cap    = 0;
static int32_t      kai_impl_count  = 0;

static inline uint32_t kai_impl_hash(int32_t proto_id, int32_t op_id, int32_t head_tag) {
    /* FNV-1a-ish mix; keys are small ints so any cheap mix is fine. */
    uint32_t h = (uint32_t) proto_id * 2654435761u;
    h ^= (uint32_t) op_id * 2246822519u;
    h ^= (uint32_t) head_tag * 40503u;
    h ^= h >> 13;
    return h;
}

/* Lookup. Returns NULL when no impl is registered for the key.
 * The dispatcher panics on NULL with a meaningful message. */
static inline void *kai_lookup_impl(int32_t proto_id, int32_t op_id, int32_t head_tag) {
    if (kai_impl_cap == 0) return NULL;
    uint32_t mask = (uint32_t) (kai_impl_cap - 1);
    uint32_t i    = kai_impl_hash(proto_id, op_id, head_tag) & mask;
    while (kai_impl_table[i].fn != NULL) {
        if (kai_impl_table[i].proto_id == proto_id &&
            kai_impl_table[i].op_id == op_id &&
            kai_impl_table[i].head_tag == head_tag) {
            return kai_impl_table[i].fn;
        }
        i = (i + 1u) & mask;
    }
    return NULL;
}

static inline void kai_impl_insert(int32_t proto_id, int32_t op_id, int32_t head_tag, void *fn) {
    uint32_t mask = (uint32_t) (kai_impl_cap - 1);
    uint32_t i    = kai_impl_hash(proto_id, op_id, head_tag) & mask;
    while (kai_impl_table[i].fn != NULL) {
        /* Duplicate registration of the same key is a codegen bug —
         * overwrite silently rather than crash. */
        if (kai_impl_table[i].proto_id == proto_id &&
            kai_impl_table[i].op_id == op_id &&
            kai_impl_table[i].head_tag == head_tag) {
            kai_impl_table[i].fn = fn;
            return;
        }
        i = (i + 1u) & mask;
    }
    kai_impl_table[i].proto_id = proto_id;
    kai_impl_table[i].op_id    = op_id;
    kai_impl_table[i].head_tag = head_tag;
    kai_impl_table[i].fn       = fn;
    kai_impl_count++;
}

/* Called once at program start by codegen-emitted main (before user
 * code runs). Sizes capacity at 2x for ~50% max load factor.
 * Idempotent: calling twice replaces the table (last writer wins),
 * which is correct for the single-compilation single-link model. */
static void kai_register_impls(const KaiImplEntry *entries, int32_t n) {
    int32_t cap = 16;
    while (cap < n * 2) cap *= 2;
    if (kai_impl_table != NULL) free(kai_impl_table);
    kai_impl_table  = (KaiImplSlot *) calloc((size_t) cap, sizeof(KaiImplSlot));
    kai_impl_cap    = cap;
    kai_impl_count  = 0;
    for (int32_t k = 0; k < n; ++k) {
        kai_impl_insert(entries[k].proto_id, entries[k].op_id, entries[k].head_tag, entries[k].fn);
    }
}

typedef struct KaiValue KaiValue;

/* Dynamic-dispatch signature used for closures and higher-order calls. */
typedef KaiValue *(*KaiFn)(KaiValue *self, KaiValue **args, int n_args);

/* Issue #440 — variant payload slot. One machine word. Mask bits in
 * `slot_mask` discriminate per-slot kind; see the `var`
 * substructure below for the encoding. Binary-compatible with
 * `KaiValue *` so legacy pointer-only callers (stage 0/1 emit,
 * immortal-variant cache hash/match, reuse-in-place memcmp) keep
 * working unchanged when mask=0. */
typedef union {
    KaiValue *ptr;
    int64_t   i64;
    double    r;
    uint32_t  c;
    int8_t    b;
} KaiVarSlot;

/* Issue #440 Phase 2 — slot kind decoder. 2 bits per slot in
 * `slot_mask`; bit pair at position (2*i) encodes slot i's kind:
 *   0 = pointer (KaiValue *) — legacy
 *   1 = Int (int64_t, .i64)
 *   2 = Real (double, .r)
 *   3 = Enum (int64_t variant_tag, .i64) — a nullary ctor of an
 *       all-nullary sum type stored as its immediate tag instead of a
 *       pointer to its interned singleton. Distinct from INT so the
 *       read-back path knows the i64 is a variant tag to re-intern
 *       (kai_enum_slot_box), not a raw integer to box (kai_int).
 * Variants with >16 slots fall back to mask=0 (all pointer) since the
 * encoding exhausts the 32-bit mask. */
#define KAI_VAR_SLOT_PTR  0u
#define KAI_VAR_SLOT_INT  1u
#define KAI_VAR_SLOT_REAL 2u
#define KAI_VAR_SLOT_ENUM 3u

static inline uint32_t kai_var_slot_kind(uint32_t mask, int i) {
    return (mask >> (2 * (uint32_t) i)) & 3u;
}

struct KaiValue {
    /* Koka-packed header (kk_header_t shape): 8 bytes, not the old 24.
     * Koka stores `{ uint8 scan_fsize; uint8 _idx; uint16 tag; refcount }`
     * in one word — kaikai mirrors it:
     *   rc          (4) — reference count
     *   tag         (1) — the KaiTag (KAI_VARIANT/INT/CONS/...); < 256 tags
     *   var_n_args  (1) — slot count for a variant (Koka's scan_fsize); a
     *                     node has ≤ 255 slots, so one byte suffices and
     *                     the per-node int32 is gone
     *   variant_tag (2) — constructor discriminant (Koka's uint16 tag)
     * `slot_mask` (the per-slot kind bits) moved OUT of the per-node header
     * into a tag→mask table (kai_slot_mask_of) — with Int now tagged, most
     * slots are kind 0 and the mask is needed only by the generic drop /
     * copy walkers, which can look it up per type instead of per node.
     * This is the 80 B → 48 B shrink: 8 B header + 5×8 slots = 48, Koka's
     * exact node size. `variant_name` likewise lives in a tag→name table. */
    int32_t  rc;
    uint8_t  tag;
    uint8_t  var_n_args;
    uint16_t variant_tag;
#ifdef KAI_TRACE_RC
    /* issue #296 — call-site attribution. Captured at kai_alloc as
     * __builtin_return_address(0); decremented from the per-site
     * histogram at kai_free_value. Only present under -DKAI_TRACE_RC=1
     * (vanilla builds keep the original two-word header — runtime
     * layout is otherwise unchanged). */
    void   *alloc_site;
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    /* Lane DIAG (#293) — kaikai-source attribution. Captured at
     * kai_alloc as the value of `kai_current_scope_fn`, set by the
     * stage-1 emitter at the top of every generated kaikai function
     * via `kai_set_scope_fn(<name>)`. Pairs with `alloc_site`
     * (return-address) to map every leaked chunk to the kaikai fn
     * whose body emitted (or should have emitted) the matching
     * decref. Only present under -DKAI_TRACE_RC_LEAKSITE=1. */
    const char *scope_fn;
#endif
    union {
        int      b;                                 /* KAI_BOOL */
        int64_t  i;                                 /* KAI_INT */
        double   r;                                 /* KAI_REAL */
        uint32_t c;                                 /* KAI_CHAR */
        uint8_t  byte_val;                                /* KAI_BYTE — Lane 4 (#473) */
        struct { size_t len; char *bytes; } s;      /* KAI_STR (heap, not NUL-terminated but we always allocate +1 byte for safety) */
        struct { KaiValue *head; KaiValue *tail; } cons;
        struct {
            int          n_fields;
            KaiValue   **fields;
            const char **names;                     /* static strings, not freed */
            int32_t      head_type_tag;             /* docs/variant-tags.md "Head-type tags" */
        } rec;
        /* Monomorphic-node packing: a variant's slots live INLINE here,
         * overlapping the union (a variant uses none of the other union
         * members). `variant_tag` / `slot_mask` moved to the header above,
         * so slot 0 no longer collides with the discriminant. This is the
         * 80 B → 48 B shrink: slots start at the union offset (8) instead
         * of after a 32 B `var` metadata substruct. Read via the
         * `kai_var_slots(v)` macro (below) — `((KaiVarSlot *)&v->as)`. The
         * slots are NOT a union member: a flexible array member in a union,
         * or in an otherwise-empty wrapper struct, is rejected by C99
         * (Linux clang errors; Apple clang silently accepts the GNU
         * extension). `n_args` is recovered from the size class / the
         * compiler-known ctor arity, not stored per node. */
        struct {
            KaiFn       fn;
            int32_t     arity;
            int32_t     n_captures;
            KaiValue  **captures;
        } clo;
        /* Opaque mutable array. Used by the stage 2 inferencer to keep
           the HM substitution indexed by TyVar id, so apply_ty lookups
           are O(1) instead of O(k) over an association list. Set
           mutates in place and returns the same value (Perceus unaware
           — callers must not alias an array across logical versions). */
        struct {
            int64_t     len;
            int64_t     cap;
            KaiValue  **items;
        } arr;
        /* KAI_REF: a single mutable cell. Owns one strong reference to
         * `cell`. ref_set drops the old cell and steals the new value;
         * ref_get hands back an incref'd copy; ref_make steals its init.
         * No length, no indexing — the invariant "a Ref is one slot" is
         * in the representation, not just the surface type. */
        struct {
            KaiValue   *cell;
        } ref;
        /* m8 #3: opaque handle to a KaiFiber. The KaiFiber struct
         * itself is heap-allocated by Spawn.spawn and owned by this
         * value: when the value's RC drops to zero, kai_free_value
         * frees the KaiFiber and decrefs its result + thunk. */
        struct KaiFiber *fib;
        /* m8 #7: opaque handle to a KaiMailbox. Pid values share the
         * mailbox's lifetime through borrowed pointers — the mailbox
         * is owned by the with_mailbox / spawn_actor helper that
         * allocated it, not by the Pid value. RC on the Pid value
         * is just a handle count; freeing a Pid does not free the
         * mailbox (that happens when the with_mailbox / spawn_actor
         * scope exits). */
        struct KaiMailbox *mb;
        /* FFI v2 (#417): opaque C handle. The parked pointer is borrowed
         * external memory — the box's RC frees only the KaiValue, never
         * `foreign_ptr` (the driver calls the C destructor itself). */
        void *foreign_ptr;
    } as;
    /* Variant slots overlap the union `as` (a variant uses none of the
     * union's named members), so a node is `8 B header + n*8 slots` = 48 B
     * for 5 slots, down from 80 B. `kai_alloc_var(n)` allocates
     * `offsetof(KaiValue, as) + n*sizeof(KaiVarSlot)` and writes slots via
     * the `kai_var_slots(v)` macro below. The slots are NOT a union member:
     * a flexible array member in a union — or in an otherwise-empty wrapper
     * struct — is a GNU extension Linux clang rejects under -std=c99. */
};

/* Variant slots for a KAI_VARIANT node. `&v->as` cast to `KaiVarSlot *`
 * IS the slot array (the slots overlap the union at offset 0). Replaces
 * the old `v->as.var.var_slots[i]` FAM; address and layout are
 * byte-for-byte identical, only the spelling changes. */
#define kai_var_slots(v) ((KaiVarSlot *) (&(v)->as))

/* ---------- Koka-style tagged-value Int representation ----------
 *
 * Ported from Koka's kklib (box.h / integer.h / kklib.h, Daan Leijen).
 * The structure kaikai never had: a value is ONE machine word. Koka,
 * box.h:26-29 — on 64-bit, using `z` for the least-significant byte:
 *
 *   xxxx xxxz   z = bbbbbbb0  : a heap pointer p (always >= 2-byte aligned)
 *   xxxx xxxz   z = bbbbbbb1  : a 63-bit value n, encoded as n*2+1
 *
 * Pointers from malloc/calloc are >= 8-byte aligned, so the bottom bit
 * is free. A small Int is therefore an immediate — no heap block, no RC
 * header. dup/drop on it are no-ops: Koka's kk_integer_dup/drop
 * (integer.h:271-278) are literally "if (is_bigint) block_dup/drop;
 * return i". This is what removes the rb-tree's 68.75M `kai_int` heap
 * allocations (75% of all allocs) and the ~800M RC ops riding on them —
 * the "kaikai-vs-Koka crux" of docs/benchmarks/rb_tree_2026-05-28.md.
 *
 * v1 uses extra_shift=0 (n*2+1, KK_TAG_BITS=1): the win is immediacy,
 * not the fused-overflow add. Out-of-range Ints (|n| > 2^62) fall back
 * to a heap KAI_INT — Koka's bigint path, identical shape. Every Int
 * accessor below understands both forms, so the boxed↔unboxed frontier
 * disappears: no re-box on a call boundary, comparison, or field read. */

#define KAI_INT_TAG_BIT  ((intptr_t) 1)

/* Bottom-bit discriminator — Koka kk_is_value / kk_is_ptr (kklib.h). */
static inline int kai_is_value(KaiValue *v) {
    return (((intptr_t) v) & KAI_INT_TAG_BIT) != 0;
}
static inline int kai_is_ptr(KaiValue *v) {
    return v != NULL && (((intptr_t) v) & KAI_INT_TAG_BIT) == 0;
}

/* Encode/decode a 63-bit immediate Int — Koka kk_integer_from_small /
 * kk_smallint_from_integer (integer.h:206-215). Arithmetic >> keeps
 * the sign. */
static inline KaiValue *kai_tagged_int(int64_t n) {
    /* Shift through uintptr_t: a signed left shift of a negative value
     * is C UB (-fsanitize=undefined trips on it). The bit pattern is
     * identical to the two's-complement signed shift, and kai_untag_int
     * uses an arithmetic `>>` to restore the sign. */
    return (KaiValue *) ((((uintptr_t) n) << 1) | (uintptr_t) KAI_INT_TAG_BIT);
}
static inline int64_t kai_untag_int(KaiValue *v) {
    return ((intptr_t) v) >> 1;
}

/* The immediate range (63-bit on a 64-bit host). */
#define KAI_SMALLINT_MAX ((int64_t) (INTPTR_MAX >> 1))
#define KAI_SMALLINT_MIN ((int64_t) (INTPTR_MIN >> 1))
static inline int kai_int_fits_immediate(int64_t n) {
    return n >= KAI_SMALLINT_MIN && n <= KAI_SMALLINT_MAX;
}

/* ---------- head-type tag derivation ---------- */

/* kai_head_tag — single-dispatch protocol dispatch key for any value.
 * See docs/variant-tags.md "Head-type tags". O(1), cache-warm hot path. */
static inline int32_t kai_head_tag(KaiValue *v) {
    if (kai_is_value(v)) return KAI_HEAD_INT;   /* immediate small Int */
    if (v == NULL) return KAI_HEAD_ANON;
    switch ((KaiTag) v->tag) {
        case KAI_UNIT:    return KAI_HEAD_UNIT;
        case KAI_BOOL:    return KAI_HEAD_BOOL;
        case KAI_INT:     return KAI_HEAD_INT;
        case KAI_REAL:    return KAI_HEAD_REAL;
        case KAI_CHAR:    return KAI_HEAD_CHAR;
        case KAI_STR:     return KAI_HEAD_STRING;
        case KAI_NIL:     return KAI_HEAD_LIST;
        case KAI_CONS:    return KAI_HEAD_LIST;
        case KAI_RECORD:  return v->as.rec.head_type_tag;
        case KAI_VARIANT: {
            int32_t vt = v->variant_tag;
            if (vt >= 0 && vt < kai_variant_to_head_len) {
                return kai_variant_to_head[vt];
            }
            return KAI_HEAD_ANON;
        }
        case KAI_CLOSURE: return KAI_HEAD_CLOSURE;
        case KAI_ARRAY:   return KAI_HEAD_ARRAY;
        case KAI_REF:     return KAI_HEAD_ANON;  /* Ref is not protocol-dispatchable */
        case KAI_FIBER:   return KAI_HEAD_FIBER;
        case KAI_PID:     return KAI_HEAD_PID;
        case KAI_FOREIGN: return KAI_HEAD_ANON;  /* opaque handle is not protocol-dispatchable (#417) */
        case KAI_BYTE:    return KAI_HEAD_BYTE;
    }
    return KAI_HEAD_ANON;
}

/* ---------- allocation and refcounting ---------- */

/* Forward declarations used across sections. */
static int       kai_op_truthy(KaiValue *v);

/* Refcount tracing (m5 #0): always-compiled counters; the per-process
   report at exit is gated on the env var KAI_TRACE_RC. The counters
   add 4 increments per kai_alloc and 2 per kai_free_value — cheap
   enough that the always-on path costs ~ns per allocation, but small
   enough that we keep them on rather than ifdef'ing them in/out
   (otherwise a measurement run would need a runtime rebuild). */
static int64_t kai_rc_alloc_total = 0;
static int64_t kai_rc_free_total  = 0;
static int64_t kai_rc_live_now    = 0;
static int64_t kai_rc_live_peak   = 0;
static int64_t kai_rc_alloc_by_tag[16] = {0};
/* issue #120 — opt-in Perceus regions. Dedicated arena counters,
 * distinct from kai_rc_alloc_total / kai_rc_free_total so a region's
 * bulk lifecycle is visible without polluting the per-value RC ledger.
 * kai_arena_alloc_total counts KaiValue headers stamped by
 * kai_arena_alloc; kai_arena_free_total counts the same headers
 * reclaimed in bulk by kai_arena_free. The two must converge at exit
 * exactly as alloc/free does — divergence flags a wrong-codegen silent
 * leak (a non-region value mistakenly arena-allocated, invisible to
 * ASAN because nothing is freed). Defined here, beside the RC ledger,
 * so kai_rc_report() / kai_rc_strict_report() (below) can read them
 * without a forward declaration; the arena machinery itself lives
 * after the singletons. */
static int64_t kai_arena_alloc_total = 0;
static int64_t kai_arena_free_total  = 0;
/* issue #118 — Perceus reuse-in-place counter. Bumped by every
 * successful in-place rewrite in kai_reuse_or_alloc_* (further down). */
static int64_t kai_rc_reuse_total = 0;
/* #2 parity probe — reuse-token DISPOSAL counter. Bumped by every
 * kai_reuse_free, i.e. every arm-top token captured (UNIQUE scrutinee
 * shell stolen) that the arm body could NOT donate to an in-frame
 * kai_variant_at and therefore had to free. In the rb-tree this is the
 * Black balance arm: `balance_left(insert_loop(l,...), ..., r)` allocates
 * its rebuilt node inside balance_left's own frame, so the stolen Black
 * cell crosses a function boundary the token cannot. This counter is the
 * exact upper bound on what an interprocedural token-pass would recover —
 * if it is small relative to alloc_total, #2 is not worth the ABI cost. */
static int64_t kai_rc_reuse_free_total = 0;
/* #2 parity probe — kai_drop_reuse_token outcome split. unique = shell
 * handed back (donatable); null_shared = rc>1 so cannot steal; null_mismatch
 * = wrong tag/arity (e.g. RBLeaf scrutinee). Tells whether the wasted fresh
 * allocs are a sharing problem (would need a different fix) or genuinely the
 * inter-frame balance case (token-pass). */
static int64_t kai_rc_tok_unique = 0;
static int64_t kai_rc_tok_null_shared = 0;
static int64_t kai_rc_tok_null_mismatch = 0;
/* Phase 1.B.1 — incref/decref call counters (the ones that actually
 * touch `rc`; pinned/INT32_MAX short-circuits are NOT counted). Lets a
 * borrow optimisation that elides incref/decref pairs show its effect
 * directly (alloc_total is unchanged by a borrow — the head is never
 * allocated, only refcounted). #812 — the increments are ALWAYS compiled
 * (parallel to kai_rc_alloc_total), gated only by the KAI_TRACE_RC env var
 * at report time; previously they sat behind -DKAI_TRACE_RC, so any binary
 * built without that define (every `kai build` output) reported 0. */
static int64_t kai_rc_incref_total = 0;
static int64_t kai_rc_decref_total = 0;

static const char *kai_rc_tag_name(int t) {
    switch (t) {
        case KAI_UNIT:    return "unit";
        case KAI_BOOL:    return "bool";
        case KAI_INT:     return "int";
        case KAI_REAL:    return "real";
        case KAI_CHAR:    return "char";
        case KAI_STR:     return "str";
        case KAI_NIL:     return "nil";
        case KAI_CONS:    return "cons";
        case KAI_RECORD:  return "record";
        case KAI_VARIANT: return "variant";
        case KAI_CLOSURE: return "closure";
        case KAI_ARRAY:   return "array";
        case KAI_REF:     return "ref";
        case KAI_BYTE:      return "Byte";
        default:          return "?";
    }
}

static void kai_rc_report(void) {
    if (!getenv("KAI_TRACE_RC")) return;
    int64_t leaked = kai_rc_alloc_total - kai_rc_free_total;
    fprintf(stderr,
        "[KAI_TRACE_RC] alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) kai_rc_alloc_total,
        (long long) kai_rc_free_total,
        (long long) leaked,
        (long long) kai_rc_live_peak);
    for (int i = 0; i < 16; i++) {
        if (kai_rc_alloc_by_tag[i] > 0) {
            fprintf(stderr, "[KAI_TRACE_RC]   tag %-7s allocs=%lld\n",
                    kai_rc_tag_name(i),
                    (long long) kai_rc_alloc_by_tag[i]);
        }
    }
    /* issue #118 — Perceus reuse-in-place counter. */
    if (kai_rc_reuse_total > 0) {
        fprintf(stderr, "[KAI_TRACE_RC]   reuse_in_place=%lld\n",
                (long long) kai_rc_reuse_total);
    }
    /* #2 parity probe — tokens captured but freed (could not donate in
     * frame). Upper bound on interprocedural-token-pass recovery. */
    if (kai_rc_reuse_free_total > 0) {
        fprintf(stderr, "[KAI_TRACE_RC]   reuse_freed=%lld\n",
                (long long) kai_rc_reuse_free_total);
    }
    /* #2 parity probe — token-drop outcome split. */
    if (kai_rc_tok_unique > 0 || kai_rc_tok_null_shared > 0 ||
        kai_rc_tok_null_mismatch > 0) {
        fprintf(stderr,
            "[KAI_TRACE_RC]   tok_unique=%lld tok_null_shared=%lld tok_null_mismatch=%lld\n",
            (long long) kai_rc_tok_unique,
            (long long) kai_rc_tok_null_shared,
            (long long) kai_rc_tok_null_mismatch);
    }
    /* Phase 1.B.1 — RC traffic (rc-touching incref/decref calls). */
    fprintf(stderr, "[KAI_TRACE_RC]   incref_total=%lld decref_total=%lld\n",
            (long long) kai_rc_incref_total,
            (long long) kai_rc_decref_total);
    /* issue #120 — region arena lifecycle. arena_live != 0 at exit
     * flags a wrong-codegen silent leak (a non-region value mistakenly
     * arena-allocated, which never frees and which ASAN cannot see). */
    if (kai_arena_alloc_total > 0 || kai_arena_free_total > 0) {
        fprintf(stderr,
            "[KAI_TRACE_RC]   arena_alloc=%lld arena_free=%lld arena_live=%lld\n",
            (long long) kai_arena_alloc_total,
            (long long) kai_arena_free_total,
            (long long) (kai_arena_alloc_total - kai_arena_free_total));
    }
}

static int kai_rc_registered = 0;
static void kai_rc_register_once(void) {
    if (kai_rc_registered) return;
    kai_rc_registered = 1;
    atexit(kai_rc_report);
}

/* Strict alloc tracing — Track #2 of issue #291.
 *
 * Build with `-DKAI_TRACE_RC=1` to enable diagnostics that surface RC
 * imbalances on macOS the same way glibc's tcache strict check does on
 * Linux (`malloc(): unaligned tcache chunk detected`). Without the
 * flag, the runtime keeps the cheap always-on counters above and adds
 * zero overhead.
 *
 * Three facilities, layered:
 *
 * 1. Per-tag free counters (kai_rc_free_by_tag). Pairs with
 *    kai_rc_alloc_by_tag so the report can show allocs/frees/live
 *    per tag at exit. live != 0 → leak. frees > allocs → double-free.
 *
 * 2. Sentinel-on-free. Just before free(v) in kai_free_value the
 *    chunk's bytes are stamped with 0xDEADBEEFDEADBEEF. macOS's
 *    malloc happily reuses the chunk, but any read through a stale
 *    pointer that was already decref'd surfaces the recognizable
 *    poison pattern (e.g. `tag = 0xEFBE...`, `rc = 0xDEADBEEF`)
 *    instead of stale-but-plausible content.
 *
 * 3. Optional per-chunk history (KAI_RC_HISTORY=1 env var). Records
 *    a small ring buffer of (chunk, op, tag) tuples covering alloc /
 *    incref / decref / free. Heavy — opt-in only — but lets a post
 *    mortem trace which tag's RC drifted.
 *
 * Reporting:
 * - Without -DKAI_TRACE_RC=1: existing env-gated KAI_TRACE_RC=1
 *   report (allocs only).
 * - With -DKAI_TRACE_RC=1: report fires unconditionally at exit
 *   unless KAI_TRACE_RC_QUIET=1 is set; per-tag allocs, frees, and
 *   live are all printed.
 */
#ifdef KAI_TRACE_RC

static int64_t kai_rc_free_by_tag[16] = {0};

#define KAI_RC_SENTINEL_U64 ((uint64_t) 0xDEADBEEFDEADBEEFULL)

/* Optional per-chunk history. Ring buffer; KAI_RC_HISTORY=1 enables
 * recording. Capacity is generous enough for the kaic2 self-compile
 * working set without ballooning memory. Indexed by (counter %
 * KAI_RC_HISTORY_CAP) so older entries get overwritten. */
#define KAI_RC_HISTORY_CAP 65536
typedef struct {
    void   *chunk;
    int32_t op;     /* 0=alloc, 1=incref, 2=decref, 3=free */
    int32_t tag;
} KaiRcHistoryEntry;
static KaiRcHistoryEntry kai_rc_history[KAI_RC_HISTORY_CAP];
static uint64_t kai_rc_history_count = 0;
static int      kai_rc_history_enabled_cached = -1; /* lazy: -1 unread, 0 off, 1 on */

static int kai_rc_history_enabled(void) {
    if (kai_rc_history_enabled_cached < 0) {
        const char *e = getenv("KAI_RC_HISTORY");
        kai_rc_history_enabled_cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return kai_rc_history_enabled_cached;
}

static void kai_rc_history_log(void *chunk, int32_t op, int32_t tag) {
    if (!kai_rc_history_enabled()) return;
    uint64_t i = kai_rc_history_count++ % KAI_RC_HISTORY_CAP;
    kai_rc_history[i].chunk = chunk;
    kai_rc_history[i].op    = op;
    kai_rc_history[i].tag   = tag;
}

static const char *kai_rc_op_name(int32_t op) {
    switch (op) {
        case 0: return "alloc";
        case 1: return "incref";
        case 2: return "decref";
        case 3: return "free";
        default: return "?";
    }
}

static void kai_rc_strict_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    int64_t leaked = kai_rc_alloc_total - kai_rc_free_total;
    fprintf(stderr,
        "[KAI_TRACE_RC] STRICT alloc_total=%lld free_total=%lld leaked=%lld live_peak=%lld\n",
        (long long) kai_rc_alloc_total,
        (long long) kai_rc_free_total,
        (long long) leaked,
        (long long) kai_rc_live_peak);
    for (int i = 0; i < 16; i++) {
        int64_t a = kai_rc_alloc_by_tag[i];
        int64_t f = kai_rc_free_by_tag[i];
        if (a == 0 && f == 0) continue;
        int64_t live = a - f;
        const char *flag =
            (live != 0)              ? " LEAK"     :
            (f > a)                  ? " DOUBLE"   : "";
        fprintf(stderr,
            "[KAI_TRACE_RC] tag=%-7s allocs=%lld frees=%lld live=%lld%s\n",
            kai_rc_tag_name(i),
            (long long) a, (long long) f, (long long) live, flag);
    }
    /* issue #120 — region arena lifecycle under strict tracing. A
     * non-zero arena_live is the ONLY visible signal of a wrong-codegen
     * silent leak: arena memory is reclaimed in bulk without a free
     * walk, so ASAN sees no use-after-free and the per-tag table above
     * never moves for arena values. */
    if (kai_arena_alloc_total > 0 || kai_arena_free_total > 0) {
        int64_t arena_live = kai_arena_alloc_total - kai_arena_free_total;
        const char *aflag = (arena_live != 0) ? " LEAK"
                          : (kai_arena_free_total > kai_arena_alloc_total) ? " DOUBLE"
                          : "";
        fprintf(stderr,
            "[KAI_TRACE_RC] arena   allocs=%lld frees=%lld live=%lld%s\n",
            (long long) kai_arena_alloc_total,
            (long long) kai_arena_free_total,
            (long long) arena_live, aflag);
    }
    if (kai_rc_history_enabled() && kai_rc_history_count > 0) {
        uint64_t total = kai_rc_history_count;
        uint64_t shown = total < KAI_RC_HISTORY_CAP ? total : KAI_RC_HISTORY_CAP;
        fprintf(stderr,
            "[KAI_TRACE_RC] history total=%llu showing last=%llu\n",
            (unsigned long long) total, (unsigned long long) shown);
        uint64_t start = total > KAI_RC_HISTORY_CAP ? total - KAI_RC_HISTORY_CAP : 0;
        for (uint64_t k = start; k < total; k++) {
            KaiRcHistoryEntry *e = &kai_rc_history[k % KAI_RC_HISTORY_CAP];
            fprintf(stderr, "[KAI_TRACE_RC] hist %s chunk=%p tag=%s\n",
                kai_rc_op_name(e->op), e->chunk,
                kai_rc_tag_name(e->tag));
        }
    }
}

static int kai_rc_strict_registered = 0;
static void kai_rc_strict_register_once(void) {
    if (kai_rc_strict_registered) return;
    kai_rc_strict_registered = 1;
    atexit(kai_rc_strict_report);
}

/* ---------- issue #296: per-call-site leak attribution ----------
 *
 * Open-addressing hash table from caller return-address to per-site
 * (alloc, free) counters. Keyed by `void *` (the return address of
 * the kai_alloc invocation, captured via __builtin_return_address(0)
 * inside each wrapper that calls kai_alloc — wrappers are marked
 * KAI_RC_NOINLINE so the address is the real emit site, not an
 * inlined parent).
 *
 * Sized at 16 K buckets — empirically the kaic2 selfhost emits
 * fewer than ~3 K distinct alloc sites, so load factor stays under
 * 20 %. Linear probing; the table is never resized. Saturates if
 * full (drops the site silently — the histogram becomes lossy
 * rather than corrupt).
 *
 * Per-chunk attribution: the alloc site is stored in the chunk's
 * `alloc_site` field (added under #ifdef KAI_TRACE_RC at the top
 * of struct KaiValue). On free, kai_free_value reads it back to
 * decrement the matching site's free counter. Cost: one extra word
 * per chunk under KAI_TRACE_RC (vanilla builds keep the original
 * two-word header). */

#define KAI_RC_SITE_BUCKETS 16384

typedef struct {
    void   *site;     /* return address; NULL = empty bucket */
    int32_t tag;      /* dominant tag observed at this site */
    int64_t allocs;
    int64_t frees;
} KaiRcSite;

static KaiRcSite kai_rc_sites[KAI_RC_SITE_BUCKETS];
static int       kai_rc_sites_count = 0;
static int       kai_rc_sites_full  = 0;  /* sticky: a probe overflowed */

/* Hash a pointer using a fast mix (Knuth-style). The high bits of
 * a return address carry the most variance, so we shift before
 * masking. */
static uint32_t kai_rc_site_hash(void *p) {
    uintptr_t x = (uintptr_t) p;
    x ^= x >> 33;
    x *= (uintptr_t) 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t) (x & (KAI_RC_SITE_BUCKETS - 1));
}

/* Find or insert. Returns NULL on overflow (table saturated).
 * Otherwise returns a pointer to the bucket (caller may mutate). */
static KaiRcSite *kai_rc_site_lookup(void *site, int32_t tag, int insert) {
    if (site == NULL) return NULL;
    uint32_t mask = KAI_RC_SITE_BUCKETS - 1;
    uint32_t h = kai_rc_site_hash(site);
    for (uint32_t i = 0; i < KAI_RC_SITE_BUCKETS; i++) {
        uint32_t k = (h + i) & mask;
        KaiRcSite *b = &kai_rc_sites[k];
        if (b->site == site) return b;
        if (b->site == NULL) {
            if (!insert) return NULL;
            b->site = site;
            b->tag  = tag;
            kai_rc_sites_count++;
            return b;
        }
    }
    kai_rc_sites_full = 1;
    return NULL;
}

static void kai_rc_site_record_alloc(void *site, int32_t tag) {
    KaiRcSite *b = kai_rc_site_lookup(site, tag, 1);
    if (b) b->allocs++;
}

static void kai_rc_site_record_free(void *site) {
    KaiRcSite *b = kai_rc_site_lookup(site, 0, 0);
    if (b) b->frees++;
}

/* qsort comparator: descending by leak count (allocs - frees). */
static int kai_rc_site_cmp_leak(const void *a, const void *b) {
    const KaiRcSite *x = (const KaiRcSite *) a;
    const KaiRcSite *y = (const KaiRcSite *) b;
    int64_t lx = x->allocs - x->frees;
    int64_t ly = y->allocs - y->frees;
    if (ly > lx) return 1;
    if (ly < lx) return -1;
    return 0;
}

/* On macOS, addresses captured via __builtin_return_address(0) are
 * post-ASLR. Print the dyld slide so post-mortem symbolization with
 * `atos -l <load>` can recover symbol names. On other platforms the
 * slide is treated as 0. */
#ifdef __APPLE__
#include <mach-o/dyld.h>
static intptr_t kai_rc_aslr_slide(void) {
    return _dyld_get_image_vmaddr_slide(0);
}
#else
static intptr_t kai_rc_aslr_slide(void) { return 0; }
#endif

static void kai_rc_site_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    if (kai_rc_sites_count == 0) return;

    /* Default top 20; KAI_TRACE_RC_TOP=N overrides. */
    int top_n = 20;
    const char *e = getenv("KAI_TRACE_RC_TOP");
    if (e && e[0]) {
        int n = atoi(e);
        if (n > 0) top_n = n;
    }

    /* Compact non-empty buckets into a contiguous array for sort. */
    KaiRcSite *flat = (KaiRcSite *) malloc(
        (size_t) kai_rc_sites_count * sizeof(KaiRcSite));
    if (!flat) return;
    int n = 0;
    int64_t total_leak = 0;
    for (int i = 0; i < KAI_RC_SITE_BUCKETS; i++) {
        if (kai_rc_sites[i].site != NULL) {
            flat[n++] = kai_rc_sites[i];
            int64_t live = kai_rc_sites[i].allocs - kai_rc_sites[i].frees;
            if (live > 0) total_leak += live;
        }
    }
    qsort(flat, (size_t) n, sizeof(KaiRcSite), kai_rc_site_cmp_leak);

    fprintf(stderr,
        "[KAI_TRACE_RC] top sites by leak (showing %d of %d distinct sites; total_leak=%lld%s)\n",
        top_n < n ? top_n : n, n, (long long) total_leak,
        kai_rc_sites_full ? "; TABLE SATURATED" : "");
    fprintf(stderr,
        "[KAI_TRACE_RC] aslr_slide=%p (subtract from site addresses for static symbolization)\n",
        (void *) kai_rc_aslr_slide());
    int limit = top_n < n ? top_n : n;
    for (int i = 0; i < limit; i++) {
        KaiRcSite *s = &flat[i];
        int64_t live = s->allocs - s->frees;
        double pct = s->allocs > 0
            ? (100.0 * (double) live / (double) s->allocs) : 0.0;
        double share = total_leak > 0
            ? (100.0 * (double) live / (double) total_leak) : 0.0;
        fprintf(stderr,
            "[KAI_TRACE_RC] site %2d %p tag=%-7s allocs=%lld frees=%lld leak=%lld leak%%=%.1f share%%=%.1f\n",
            i + 1, s->site, kai_rc_tag_name(s->tag),
            (long long) s->allocs, (long long) s->frees, (long long) live,
            pct, share);
    }
    free(flat);
}

static int kai_rc_site_registered = 0;
static void kai_rc_site_register_once(void) {
    if (kai_rc_site_registered) return;
    kai_rc_site_registered = 1;
    atexit(kai_rc_site_report);
}

/* Wrappers that ultimately call kai_alloc must NOT be inlined when
 * tracing is enabled — otherwise __builtin_return_address(0) inside
 * the wrapper points at the wrapper's parent's parent, not the
 * real emit site. Vanilla builds drop the attribute (zero overhead). */
#define KAI_RC_NOINLINE __attribute__((noinline))

/* ---------- Lane DIAG (#293): per-leak-site attribution ----------
 *
 * Aim: map every leaked chunk back to the kaikai source function that
 * allocated it, so the fix lane (Lane FIX) can rank scope_fns by leak
 * volume and patch the missing decrefs in their emit shapes.
 *
 * Two pieces of state at alloc time:
 *  - `kai_current_scope_fn`: the kaikai fn currently executing,
 *    set by the emitter via `kai_set_scope_fn(<name>)` at the top
 *    of every generated body. Static (the kaic2 self-compile is
 *    single-threaded). Inherits from the caller for fns that don't
 *    yet carry the hook (dispatch from prelude / FFI), so the
 *    attribution remains stable across mixed call paths.
 *  - allocator tag (KAI_VARIANT, KAI_RECORD, …): captured from
 *    `kai_alloc_traced`'s `tag` parameter.
 *
 * Aggregation is keyed on `(scope_fn, tag)`. Per-chunk metadata stays
 * to one extra pointer (`scope_fn` field on KaiValue under
 * KAI_TRACE_RC_LEAKSITE); the agg table is a fixed open-addressed
 * hash sized at 8 K buckets — empirically the kaic2 self-compile
 * has ≤ ~600 unique kaikai fn names, so worst-case load factor is
 * ~8 % across (fn × 13 tags). */
#ifdef KAI_TRACE_RC_LEAKSITE

static const char *kai_current_scope_fn = "<root>";

static void kai_set_scope_fn(const char *name) {
    kai_current_scope_fn = (name != NULL) ? name : "<root>";
}

#define KAI_LEAKSITE_BUCKETS 8192

typedef struct {
    const char *scope_fn;   /* NULL = empty bucket */
    int32_t     tag;
    int64_t     allocs;
    int64_t     frees;
} KaiLeakSite;

static KaiLeakSite kai_leaksites[KAI_LEAKSITE_BUCKETS];
static int         kai_leaksites_count = 0;
static int         kai_leaksites_full  = 0;

static uint32_t kai_leaksite_hash(const char *scope, int32_t tag) {
    uintptr_t x = (uintptr_t) scope;
    x ^= (uintptr_t) ((uint32_t) tag * 0x9E3779B1u);
    x ^= x >> 33;
    x *= (uintptr_t) 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return (uint32_t) (x & (KAI_LEAKSITE_BUCKETS - 1));
}

static KaiLeakSite *kai_leaksite_lookup(const char *scope, int32_t tag, int insert) {
    if (scope == NULL) scope = "<unknown>";
    uint32_t mask = KAI_LEAKSITE_BUCKETS - 1;
    uint32_t h = kai_leaksite_hash(scope, tag);
    for (uint32_t i = 0; i < KAI_LEAKSITE_BUCKETS; i++) {
        uint32_t k = (h + i) & mask;
        KaiLeakSite *b = &kai_leaksites[k];
        if (b->scope_fn == scope && b->tag == tag) return b;
        if (b->scope_fn == NULL) {
            if (!insert) return NULL;
            b->scope_fn = scope;
            b->tag      = tag;
            kai_leaksites_count++;
            return b;
        }
    }
    kai_leaksites_full = 1;
    return NULL;
}

static void kai_leaksite_record_alloc(const char *scope, int32_t tag) {
    KaiLeakSite *b = kai_leaksite_lookup(scope, tag, 1);
    if (b) b->allocs++;
}

static void kai_leaksite_record_free(const char *scope, int32_t tag) {
    KaiLeakSite *b = kai_leaksite_lookup(scope, tag, 0);
    if (b) b->frees++;
}

static const char *kai_leaksite_alloc_fn(int32_t tag) {
    switch (tag) {
        case KAI_VARIANT: return "kai_variant";
        case KAI_RECORD:  return "kai_record";
        case KAI_CONS:    return "kai_cons";
        case KAI_STR:     return "kai_str";
        case KAI_INT:     return "kai_int";
        case KAI_REAL:    return "kai_real";
        case KAI_CHAR:    return "kai_char";
        case KAI_CLOSURE: return "kai_closure";
        case KAI_ARRAY:   return "kai_array";
        case KAI_BOOL:    return "kai_bool";
        case KAI_UNIT:    return "kai_unit";
        case KAI_NIL:     return "kai_nil";
        case KAI_FIBER:   return "kai_fiber";
        case KAI_PID:     return "kai_pid";
        case KAI_BYTE:      return "kai_byte";
        default:          return "kai_alloc";
    }
}

static int kai_leaksite_cmp_leak(const void *a, const void *b) {
    const KaiLeakSite *x = (const KaiLeakSite *) a;
    const KaiLeakSite *y = (const KaiLeakSite *) b;
    int64_t lx = x->allocs - x->frees;
    int64_t ly = y->allocs - y->frees;
    if (ly > lx) return 1;
    if (ly < lx) return -1;
    return 0;
}

static void kai_leaksite_report(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    if (kai_leaksites_count == 0) return;

    int top_n = 20;
    const char *e = getenv("KAI_TRACE_RC_LEAKSITE_TOP");
    if (e && e[0]) {
        int n = atoi(e);
        if (n > 0) top_n = n;
    }

    KaiLeakSite *flat = (KaiLeakSite *) malloc(
        (size_t) kai_leaksites_count * sizeof(KaiLeakSite));
    if (!flat) return;
    int n = 0;
    int64_t total_leak = 0;
    int64_t per_tag_leak[16] = {0};
    for (int i = 0; i < KAI_LEAKSITE_BUCKETS; i++) {
        if (kai_leaksites[i].scope_fn != NULL) {
            flat[n++] = kai_leaksites[i];
            int64_t live = kai_leaksites[i].allocs - kai_leaksites[i].frees;
            if (live > 0) {
                total_leak += live;
                int t = kai_leaksites[i].tag;
                if (t >= 0 && t < 16) per_tag_leak[t] += live;
            }
        }
    }
    qsort(flat, (size_t) n, sizeof(KaiLeakSite), kai_leaksite_cmp_leak);

    int limit = top_n < n ? top_n : n;
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] top sites by leak (showing %d of %d distinct (scope_fn,tag); total_leak=%lld%s)\n",
        limit, n, (long long) total_leak,
        kai_leaksites_full ? "; TABLE SATURATED" : "");
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] columns: rank | alloc_fn | scope_fn | allocs | frees | leak | leak%% | share%%\n");
    for (int i = 0; i < limit; i++) {
        KaiLeakSite *s = &flat[i];
        int64_t live = s->allocs - s->frees;
        double pct = s->allocs > 0
            ? (100.0 * (double) live / (double) s->allocs) : 0.0;
        double share = total_leak > 0
            ? (100.0 * (double) live / (double) total_leak) : 0.0;
        fprintf(stderr,
            "[KAI_TRACE_RC_LEAKSITE] %2d | %-11s | %-40s | %10lld | %10lld | %10lld | %5.1f | %5.1f\n",
            i + 1,
            kai_leaksite_alloc_fn(s->tag),
            s->scope_fn,
            (long long) s->allocs,
            (long long) s->frees,
            (long long) live,
            pct, share);
    }
    /* Per-tag totals so checkpoint 2 (sum-by-alloc_fn vs per-tag totals)
     * can be verified at a glance from the same dump. */
    fprintf(stderr,
        "[KAI_TRACE_RC_LEAKSITE] per-tag leak totals (sum across all scope_fns):\n");
    for (int t = 0; t < 16; t++) {
        if (per_tag_leak[t] != 0) {
            fprintf(stderr,
                "[KAI_TRACE_RC_LEAKSITE]   tag=%-7s leak=%lld\n",
                kai_rc_tag_name(t), (long long) per_tag_leak[t]);
        }
    }
    free(flat);
}

static int kai_leaksite_registered = 0;
static void kai_leaksite_register_once(void) {
    if (kai_leaksite_registered) return;
    kai_leaksite_registered = 1;
    atexit(kai_leaksite_report);
}

#endif /* KAI_TRACE_RC_LEAKSITE */

#endif /* KAI_TRACE_RC */

#ifndef KAI_TRACE_RC
#define KAI_RC_NOINLINE
#endif

/* ---------- KAI_PROFILE_RC — per-category wall breakdown (lane #426)
 *
 * Independent of KAI_TRACE_RC. Times the four hot RC functions with
 * `clock_gettime(CLOCK_MONOTONIC)` and prints a per-category summary
 * at exit. Used to attribute the 16× C wall on the RB-tree benchmark
 * (docs/benchmarks/rb_tree_2026-05-09.md) to alloc / free / non-free
 * RC traffic, so the team can scope Phase 4 (variant-field unboxing)
 * vs drop-specialisation correctly.
 *
 * Build with `-DKAI_PROFILE_RC=1` to compile the wrappers in. Output
 * gates on env var KAI_PROFILE_RC=1 at run time so a single binary
 * can be timed with and without the report. Vanilla builds compile
 * to empty hooks and add zero overhead.
 *
 * Categories:
 *   - alloc    — time inside `kai_alloc` (the leaf calloc + bookkeep).
 *   - free     — time inside `kai_free_value` (the actual free + the
 *                cascading decrefs on contained children).
 *   - decref   — time inside `kai_decref` for every call that reaches
 *                the rc-- path (skips NULL and singleton early exits).
 *                Includes time spent calling `kai_free_value`; subtract
 *                the free total to get pure non-free RC traffic.
 *   - incref   — time inside `kai_incref` for every call that reaches
 *                the rc++ path (skips NULL and singleton early exits).
 *
 * clock_gettime(CLOCK_MONOTONIC) costs ~30 ns per call on macOS — at
 * ~25 M decrefs the instrumentation inflates the wall by ~1.5 s on
 * the RB-tree benchmark. Per-category PROPORTIONS remain robust;
 * absolute milliseconds under -DKAI_PROFILE_RC are not directly
 * comparable to the un-instrumented wall. The exit report prints the
 * instrumented wall alongside the categories so the gap is visible.
 *
 * Match dispatch is NOT a separate category — pattern test and
 * field-extract are emitted inline by the codegen (see
 * `_scr->variant_tag` reads in compiler-emitted C), so there
 * is no central function to wrap. It folds into the un-attributed
 * "other" bucket alongside everything else (calls, arithmetic,
 * stack management).
 */
#ifdef KAI_PROFILE_RC
#include <time.h>

static int64_t kai_prof_alloc_ns   = 0;
static int64_t kai_prof_free_ns    = 0;
static int64_t kai_prof_incref_ns  = 0;
static int64_t kai_prof_decref_ns  = 0;
static int64_t kai_prof_alloc_n    = 0;
static int64_t kai_prof_free_n     = 0;
static int64_t kai_prof_incref_n   = 0;
static int64_t kai_prof_decref_n   = 0;
static int64_t kai_prof_decref_to_zero_n = 0;
static struct timespec kai_prof_t0;
static int kai_prof_init_done = 0;
static int kai_prof_enabled = 0;

/* Exclusive-time stack. The four hot RC functions call each other
 * (kai_decref → kai_free_value → kai_decref → ...). Naive timing
 * double-counts nested time. To get exclusive (self-only) time per
 * category, every active call records its start_ns and accumulates
 * the gross duration of any child instrumented call into a `child_ns`
 * slot; on exit, exclusive = (now - start) - child_ns is added to the
 * category counter, and gross time is bubbled to the parent's slot.
 *
 * Stack depth 32 is overkill — the deepest realistic chain is decref
 * → free_value → decref → free_value → ... bounded by tree depth (~30
 * for 1M nodes). Static fixed array; saturates on overflow (very
 * defensively — if it ever fires we silently mis-attribute, no crash). */
#define KAI_PROF_STACK_CAP 64
typedef struct {
    int64_t start_ns;
    int64_t child_ns;
} KaiProfFrame;
static KaiProfFrame kai_prof_stack[KAI_PROF_STACK_CAP];
static int kai_prof_sp = 0;

static inline int64_t kai_prof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

static inline int kai_prof_push(void) {
    if (kai_prof_sp >= KAI_PROF_STACK_CAP) return -1;  /* saturate */
    int idx = kai_prof_sp++;
    kai_prof_stack[idx].start_ns = kai_prof_now_ns();
    kai_prof_stack[idx].child_ns = 0;
    return idx;
}

static inline int64_t kai_prof_pop_exclusive(int idx) {
    if (idx < 0) return 0;
    int64_t end = kai_prof_now_ns();
    int64_t gross = end - kai_prof_stack[idx].start_ns;
    int64_t self  = gross - kai_prof_stack[idx].child_ns;
    if (self < 0) self = 0;  /* clock noise can produce tiny negatives */
    kai_prof_sp = idx;  /* unwind any deeper saturated frames defensively */
    /* Bubble gross time to parent so its exclusive subtraction works. */
    if (idx > 0) kai_prof_stack[idx - 1].child_ns += gross;
    return self;
}

static void kai_prof_report(void) {
    if (!getenv("KAI_PROFILE_RC")) return;
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t wall_ns =
        ((int64_t) t1.tv_sec - (int64_t) kai_prof_t0.tv_sec) * 1000000000LL
      + ((int64_t) t1.tv_nsec - (int64_t) kai_prof_t0.tv_nsec);
    int64_t alloc_ns  = kai_prof_alloc_ns;
    int64_t free_ns   = kai_prof_free_ns;
    int64_t incref_ns = kai_prof_incref_ns;
    int64_t decref_ns = kai_prof_decref_ns;
    /* Categories are exclusive (self-only) — no overlap. */
    int64_t rc_traffic_ns = incref_ns + decref_ns;
    int64_t accounted_ns  = alloc_ns + free_ns + rc_traffic_ns;
    int64_t other_ns      = wall_ns - accounted_ns;
    fprintf(stderr,
        "[KAI_PROFILE_RC] wall_ms=%lld alloc_ms=%lld free_ms=%lld "
        "rc_traffic_ms=%lld other_ms=%lld\n",
        (long long) (wall_ns / 1000000),
        (long long) (alloc_ns / 1000000),
        (long long) (free_ns  / 1000000),
        (long long) (rc_traffic_ns / 1000000),
        (long long) (other_ns / 1000000));
    fprintf(stderr,
        "[KAI_PROFILE_RC] decref_ms=%lld (self) incref_ms=%lld (self)\n",
        (long long) (decref_ns / 1000000),
        (long long) (incref_ns / 1000000));
    fprintf(stderr,
        "[KAI_PROFILE_RC] calls alloc=%lld free=%lld incref=%lld "
        "decref=%lld decref_to_zero=%lld\n",
        (long long) kai_prof_alloc_n,
        (long long) kai_prof_free_n,
        (long long) kai_prof_incref_n,
        (long long) kai_prof_decref_n,
        (long long) kai_prof_decref_to_zero_n);
    if (wall_ns > 0) {
        fprintf(stderr,
            "[KAI_PROFILE_RC] share alloc=%.1f%% free=%.1f%% "
            "rc_traffic=%.1f%% other=%.1f%%\n",
            100.0 * alloc_ns / wall_ns,
            100.0 * free_ns / wall_ns,
            100.0 * rc_traffic_ns / wall_ns,
            100.0 * other_ns / wall_ns);
    }
}

static void kai_prof_init(void) {
    if (kai_prof_init_done) return;
    kai_prof_init_done = 1;
    const char *e = getenv("KAI_PROFILE_RC");
    kai_prof_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    if (!kai_prof_enabled) return;
    clock_gettime(CLOCK_MONOTONIC, &kai_prof_t0);
    atexit(kai_prof_report);
}

#define KAI_PROF_ENTER()           \
    int _kai_prof_idx = -1;        \
    do {                           \
        if (!kai_prof_init_done) kai_prof_init(); \
        if (kai_prof_enabled) _kai_prof_idx = kai_prof_push(); \
    } while (0)

#define KAI_PROF_EXIT(category)                                              \
    do {                                                                     \
        if (kai_prof_enabled) {                                              \
            kai_prof_##category##_ns += kai_prof_pop_exclusive(_kai_prof_idx); \
            kai_prof_##category##_n++;                                       \
        }                                                                    \
    } while (0)

#else /* !KAI_PROFILE_RC */

#define KAI_PROF_ENTER()        ((void) 0)
#define KAI_PROF_EXIT(category) ((void) 0)

#endif /* KAI_PROFILE_RC */

/* ---------- variant_name histogram (issue #300, lane #297/#298 validation)
 *
 * Independent of KAI_TRACE_RC. Counts variant alloc/free traffic keyed on
 * the `variant_name` string-literal pointer (stable across runs, immune to
 * the tail-call ABI bug that contaminates address-keyed attribution).
 *
 * Opt-in: build with `-DKAI_TRACE_VAR_NAMES=1`. Vanilla builds compile to
 * empty hooks; emitted IR (and selfhost byte-identity) is unchanged.
 *
 * Output: at exit a destructor dumps `[VAR_NAME] <name> allocs=A frees=F
 * leak=A-F` lines sorted by leak desc to stderr. Suppressed by
 * KAI_TRACE_RC_QUIET=1 (shared with the address-keyed tracer).
 */
#ifdef KAI_TRACE_VAR_NAMES

#define KAI_VAR_NAME_BUCKETS 4096

typedef struct {
    const char *name;
    int64_t allocs;       /* invocation count: every kai_variant_u(_, name, ...) call */
    int64_t real_allocs;  /* physical allocs: invocations that miss singleton/immortal cache */
    int64_t frees;
} KaiVarNameBucket;

static KaiVarNameBucket kai_var_names[KAI_VAR_NAME_BUCKETS];
static int kai_var_names_count;

static KaiVarNameBucket *kai_var_name_lookup(const char *name) {
    if (name == NULL) return NULL;
    uintptr_t k = (uintptr_t) name;
    uint32_t h = (uint32_t) ((k ^ (k >> 16)) * 0x9e3779b1u);
    for (int probe = 0; probe < KAI_VAR_NAME_BUCKETS; ++probe) {
        int i = (int) ((h + probe) & (KAI_VAR_NAME_BUCKETS - 1));
        if (kai_var_names[i].name == NULL) {
            kai_var_names[i].name = name;
            kai_var_names_count++;
            return &kai_var_names[i];
        }
        if (kai_var_names[i].name == name) return &kai_var_names[i];
    }
    return NULL; /* table full — silently drop */
}

static void kai_var_name_record_alloc(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->allocs++;
}

static void kai_var_name_record_real_alloc(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->real_allocs++;
}

static void kai_var_name_record_free(const char *name) {
    KaiVarNameBucket *b = kai_var_name_lookup(name);
    if (b) b->frees++;
}

static int kai_var_name_cmp_leak_desc(const void *a, const void *b) {
    const KaiVarNameBucket *x = (const KaiVarNameBucket *) a;
    const KaiVarNameBucket *y = (const KaiVarNameBucket *) b;
    int64_t lx = x->real_allocs - x->frees;
    int64_t ly = y->real_allocs - y->frees;
    if (ly != lx) return (ly > lx) - (ly < lx);
    return (y->real_allocs > x->real_allocs) - (y->real_allocs < x->real_allocs);
}

__attribute__((destructor))
static void kai_var_name_report_at_exit(void) {
    if (getenv("KAI_TRACE_RC_QUIET")) return;
    KaiVarNameBucket snapshot[KAI_VAR_NAME_BUCKETS];
    int n = 0;
    for (int i = 0; i < KAI_VAR_NAME_BUCKETS; ++i) {
        if (kai_var_names[i].name != NULL) snapshot[n++] = kai_var_names[i];
    }
    if (n == 0) return;
    qsort(snapshot, (size_t) n, sizeof(snapshot[0]), kai_var_name_cmp_leak_desc);
    int64_t total_invs = 0, total_real = 0, total_frees = 0;
    for (int i = 0; i < n; ++i) {
        total_invs += snapshot[i].allocs;
        total_real += snapshot[i].real_allocs;
        total_frees += snapshot[i].frees;
    }
    fprintf(stderr,
        "[VAR_NAME] total distinct=%d invocations=%lld real_allocs=%lld frees=%lld leak=%lld\n",
        n, (long long) total_invs, (long long) total_real,
        (long long) total_frees, (long long) (total_real - total_frees));
    fprintf(stderr,
        "[VAR_NAME] columns: name | invocations | real_allocs | frees | leak\n");
    for (int i = 0; i < n; ++i) {
        int64_t leak = snapshot[i].real_allocs - snapshot[i].frees;
        fprintf(stderr,
            "[VAR_NAME] %-24s inv=%-12lld real=%-10lld frees=%-10lld leak=%lld\n",
            snapshot[i].name,
            (long long) snapshot[i].allocs,
            (long long) snapshot[i].real_allocs,
            (long long) snapshot[i].frees,
            (long long) leak);
    }
}

#define KAI_VAR_NAME_ALLOC(name)      kai_var_name_record_alloc(name)
#define KAI_VAR_NAME_REAL_ALLOC(name) kai_var_name_record_real_alloc(name)
#define KAI_VAR_NAME_FREE(name)       kai_var_name_record_free(name)

#else /* !KAI_TRACE_VAR_NAMES */

#define KAI_VAR_NAME_ALLOC(name)      ((void) 0)
#define KAI_VAR_NAME_REAL_ALLOC(name) ((void) 0)
#define KAI_VAR_NAME_FREE(name)       ((void) 0)

#endif /* KAI_TRACE_VAR_NAMES */

/* Monomorphic-node packing: the variant name is no longer stored per
 * node (it was 8 B/node of metadata the hot path never reads). Instead a
 * process-global tag→name table is populated the first time each tag is
 * constructed (the ctor still passes `name`), and the rare readers
 * (to_string, deep_copy, protocol dispatch) recover it via
 * `kai_variant_name_of(tag)`. Tags are dense small ints; a flat array
 * with a lazy grow covers them. Names are static string literals, never
 * freed. */
#define KAI_VARNAME_TABLE_INIT 256
static const char **kai_varname_table = NULL;
static int kai_varname_table_cap = 0;

static void kai_varname_register(int32_t tag, const char *name) {
    if (tag < 0 || name == NULL) return;
    if (tag >= kai_varname_table_cap) {
        int newcap = kai_varname_table_cap == 0 ? KAI_VARNAME_TABLE_INIT : kai_varname_table_cap;
        while (tag >= newcap) newcap *= 2;
        kai_varname_table = (const char **) realloc(kai_varname_table, (size_t) newcap * sizeof(char *));
        for (int i = kai_varname_table_cap; i < newcap; ++i) kai_varname_table[i] = NULL;
        kai_varname_table_cap = newcap;
    }
    if (kai_varname_table[tag] == NULL) kai_varname_table[tag] = name;
}

static const char *kai_variant_name_of(int32_t tag) {
    if (tag >= 0 && tag < kai_varname_table_cap && kai_varname_table[tag] != NULL) {
        return kai_varname_table[tag];
    }
    return "";
}

/* Koka-packed header: the per-slot kind bits (`slot_mask`) no longer live
 * per node — they are a property of the constructor, so a tag→mask table
 * holds them (populated at first construction, like the name table). The
 * generic drop / copy / reuse walkers read `kai_slot_mask_of(tag)`. A tag
 * never seen returns 0 (all-pointer), the conservative default. */
static uint32_t *kai_slotmask_table = NULL;
static int kai_slotmask_table_cap = 0;
static uint8_t *kai_slotmask_seen = NULL;   /* 1 once a tag's mask is recorded */

static void kai_slotmask_register(int32_t tag, uint32_t mask) {
    if (tag < 0) return;
    if (tag >= kai_slotmask_table_cap) {
        int newcap = kai_slotmask_table_cap == 0 ? KAI_VARNAME_TABLE_INIT : kai_slotmask_table_cap;
        while (tag >= newcap) newcap *= 2;
        kai_slotmask_table = (uint32_t *) realloc(kai_slotmask_table, (size_t) newcap * sizeof(uint32_t));
        kai_slotmask_seen  = (uint8_t *)  realloc(kai_slotmask_seen,  (size_t) newcap * sizeof(uint8_t));
        for (int i = kai_slotmask_table_cap; i < newcap; ++i) { kai_slotmask_table[i] = 0; kai_slotmask_seen[i] = 0; }
        kai_slotmask_table_cap = newcap;
    }
    if (!kai_slotmask_seen[tag]) { kai_slotmask_table[tag] = mask; kai_slotmask_seen[tag] = 1; }
}

static uint32_t kai_slot_mask_of(int32_t tag) {
    if (tag >= 0 && tag < kai_slotmask_table_cap) return kai_slotmask_table[tag];
    return 0;
}

/* ---------- KAI_MAX_HEAP: process heap ceiling (host containment) ----------
 *
 * Caps total committed heap so a runaway aborts clean instead of dragging
 * the host into an OOM hang (on macOS the RAM compressor masks exhaustion;
 * RLIMIT_AS / `ulimit -v` are no-ops, so there is no OS ceiling at all).
 * Set `KAI_MAX_HEAP` to a byte count or a k/m/g-suffixed size (e.g. `4g`).
 * Unset/empty/unparseable -> no cap, one predicted branch per grow point.
 *
 * The counter is monotonic high-water (commit, not live): the value heap
 * never returns slabs to the OS, so the running total is the process's
 * committed footprint, which is the metric containment cares about. It is
 * charged at every OS-commit grow point — slab grow, oversized slab, cell
 * calloc, variant-block fallback, arena chunk, and string/array payloads —
 * so no allocation path can grow past the ceiling uncounted. Single OS
 * thread (fibers share it), so a plain global needs no synchronisation. */
static size_t kai_heap_limit_cached = 0;   /* 0 = no cap */
static int    kai_heap_inited       = 0;
static size_t kai_heap_committed    = 0;

static size_t kai_heap_limit(void) {
    if (!kai_heap_inited) {
        kai_heap_inited = 1;
        const char *raw = getenv("KAI_MAX_HEAP");
        if (raw && *raw) {
            char *end = NULL;
            unsigned long long n = strtoull(raw, &end, 10);
            unsigned long long mul = 1;
            if (end && *end) {
                switch (*end) {
                    case 'k': case 'K': mul = 1024ULL; break;
                    case 'm': case 'M': mul = 1024ULL * 1024ULL; break;
                    case 'g': case 'G': mul = 1024ULL * 1024ULL * 1024ULL; break;
                    default: mul = 0; break;   /* junk suffix -> no cap */
                }
                if (mul && end[1]) mul = 0;    /* trailing junk after suffix -> no cap */
            }
            if (mul && n > 0 && n <= (unsigned long long) ((size_t) -1) / mul) {
                kai_heap_limit_cached = (size_t) (n * mul);
            }
        }
    }
    return kai_heap_limit_cached;
}

static void kai_heap_charge(size_t sz) {
    size_t limit = kai_heap_limit();
    if (limit && kai_heap_committed + sz > limit) {
        fprintf(stderr,
            "kai: heap limit exceeded (KAI_MAX_HEAP=%s, used %zu bytes)\n",
            getenv("KAI_MAX_HEAP"), kai_heap_committed);
        exit(1);
    }
    kai_heap_committed += sz;
}

/* malloc/realloc that charge the payload against the ceiling and fold in
 * the OOM check shared by every value-heap allocation. */
static void *kai_heap_malloc(size_t sz) {
    kai_heap_charge(sz);
    void *p = malloc(sz);
    if (!p) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    return p;
}

static void *kai_heap_realloc(void *ptr, size_t old_sz, size_t new_sz) {
    if (new_sz > old_sz) kai_heap_charge(new_sz - old_sz);
    void *p = realloc(ptr, new_sz);
    if (!p) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    return p;
}

#ifdef KAI_TRACE_RC
/* Under trace/profile the cell + slot free-lists are disabled (they
 * would perturb leak attribution and poisoning). Provide malloc-only
 * slot helpers here so the variant constructors below link in this
 * mode too — the pooled versions live in the #else branch. */
static KaiVarSlot *kai_slots_alloc(int n) {
    return (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
}
static void kai_slots_free(KaiVarSlot *slots, int n) { (void) n; free(slots); }
static KaiValue *kai_alloc_traced(KaiTag tag, void *site) {
#else
/* Issue #118 follow-up #3 — fixed-size cell free-list (Koka/mimalloc
 * model). Every KaiValue is the same size, so a freed cell can host the
 * next allocation without a malloc/free round-trip. This is the
 * size-keyed reuse Koka gets implicitly from its heap: the functional
 * rebuild's discarded spine cells are recycled by the very next
 * constructor of the same size, WITHOUT any compiler-level token
 * threading. Bounded so a transient spike does not pin memory forever.
 *
 * Disabled under KAI_TRACE_RC / KAI_PROFILE_RC: those modes poison freed
 * cells and rely on exact malloc/free pairing for leak attribution, so
 * recycling would corrupt their bookkeeping. The free-list is a pure
 * production-path allocator optimisation — RC semantics and emitted code
 * are byte-identical; only the malloc/free traffic changes. */
#if !defined(KAI_TRACE_RC) && !defined(KAI_PROFILE_RC) && !defined(KAI_NO_CELL_POOL)
#define KAI_CELL_POOL_ACTIVE 1
/* Cap sized for the functional-rebuild working set. The rb-tree churns
 * a ~1M-cell live set with a deep free/realloc cycle; a cap below the
 * peak free-list depth spills to libc and the wall destabilises around
 * the 10× target (measured: 262Ki oscillates 9.9–10.3× by run, 1Mi
 * holds a stable 9.8–9.9×). 1Mi entries (8 MB of pointers, lazy .bss)
 * keep the recycle hit-rate high; beyond it we fall through to
 * malloc/free. */
#define KAI_CELL_POOL_CAP 1048576
static KaiValue *kai_cell_pool[KAI_CELL_POOL_CAP];
static int kai_cell_pool_n = 0;

/* Companion free-list for the variant `slots[]` arrays, keyed by arity
 * (the dominant per-node malloc alongside the cell header). Arity 1..8
 * covers every variant the functional rebuild churns; larger arities
 * fall through to malloc/free. Sized to match the cell pool's working
 * set so slot reuse does not become the spill bottleneck. */
#define KAI_SLOT_POOL_MAXN 8
#define KAI_SLOT_POOL_CAP  1048576
static KaiVarSlot *kai_slot_pool[KAI_SLOT_POOL_MAXN + 1][KAI_SLOT_POOL_CAP];
static int kai_slot_pool_n[KAI_SLOT_POOL_MAXN + 1];

/* FAM lane: free-list for whole variant BLOCKS (header + n inline slots
 * in one allocation), keyed by arity. Replaces the cell_pool + slot_pool
 * pair for variants — one push/pop instead of two, one cache line per
 * node instead of a header here and a slots[] array somewhere else.
 * Arity 0..8 covers every variant the functional rebuild churns; larger
 * arities fall through to malloc/free. A pooled block already has the
 * right byte size for its arity, so reuse is size-matched by
 * construction (the reuse recognisers only ever rewrite same-arity). */
#define KAI_VAR_BLOCK_POOL_MAXN 8
#define KAI_VAR_BLOCK_POOL_CAP  1048576
static KaiValue *kai_var_block_pool[KAI_VAR_BLOCK_POOL_MAXN + 1][KAI_VAR_BLOCK_POOL_CAP];
static int kai_var_block_pool_n[KAI_VAR_BLOCK_POOL_MAXN + 1];

/* ---------- variant-block slab allocator (issue: malloc 6.2% of bench) ----------
 *
 * A fresh variant block used to be `malloc(kai_var_block_size(n))` — one
 * libc allocation per node, 6.96M of them on the rb-tree bench (the tree
 * grows monotonically, so the block pool stays empty during the insert
 * phase and every new node is a real malloc). Idea adapted from Koka's
 * mimalloc segments — amortise the allocator, never hand individual cells
 * back to libc — but WITHOUT a dependency: a plain bump allocator over
 * malloc'd slabs.
 *
 * Design (slab-only, per asu): EVERY variant block comes from a slab.
 * `kai_slab_alloc(sz)` bump-allocates `sz` bytes (8-aligned) from the
 * current slab, growing a new slab when the bump pointer would overrun.
 * The free path (kai_var_block_free) NEVER calls libc free on an
 * individual block — a slab-interior pointer is not a malloc'd address,
 * so free() on it is UB/corruption. Instead a freed block goes to the
 * arity-keyed block pool (the free-list that already exists); a pool-full
 * or over-arity spill is simply DROPPED (the cell stays in its slab,
 * reclaimed when the whole slab is freed at exit — not an observable
 * leak). The slabs themselves are tracked and freed in kai_slab_teardown
 * (registered via atexit) so ASAN sees still-reachable == 0.
 *
 * Soundness net: if any free() ever reaches a slab-interior pointer, ASAN
 * fires "free on non-malloc'd address" immediately. The self-compile gate
 * (kaic2b.c == kaic2c.c) proves no AST node — of any arity — was
 * corrupted by the size-classing. */
#define KAI_SLAB_SIZE (256u * 1024u)   /* 256 KiB per slab */
static char  *kai_slab_cur    = NULL;  /* current slab base */
static size_t kai_slab_off    = 0;     /* bump offset into current slab */
static char **kai_slab_list   = NULL;  /* all slabs, for teardown */
static int    kai_slab_count  = 0;
static int    kai_slab_cap    = 0;
static int    kai_slab_atexit = 0;

static void kai_slab_teardown(void) {
    for (int i = 0; i < kai_slab_count; ++i) free(kai_slab_list[i]);
    free(kai_slab_list);
    kai_slab_list = NULL; kai_slab_count = 0; kai_slab_cap = 0;
    kai_slab_cur = NULL; kai_slab_off = 0;
}

static void *kai_slab_alloc(size_t sz) {
    sz = (sz + 7u) & ~(size_t) 7u;            /* 8-byte align */
    if (sz > KAI_SLAB_SIZE) return kai_heap_malloc(sz); /* oversized: standalone (never freed individually either) */
    if (!kai_slab_cur || kai_slab_off + sz > KAI_SLAB_SIZE) {
        kai_heap_charge(KAI_SLAB_SIZE);
        char *slab = (char *) malloc(KAI_SLAB_SIZE);
        if (!slab) { fprintf(stderr, "kai: out of memory (slab)\n"); exit(1); }
        if (kai_slab_count == kai_slab_cap) {
            int ncap = kai_slab_cap == 0 ? 64 : kai_slab_cap * 2;
            kai_slab_list = (char **) realloc(kai_slab_list, (size_t) ncap * sizeof(char *));
            kai_slab_cap = ncap;
        }
        kai_slab_list[kai_slab_count++] = slab;
        kai_slab_cur = slab;
        kai_slab_off = 0;
        if (!kai_slab_atexit) { atexit(kai_slab_teardown); kai_slab_atexit = 1; }
    }
    void *p = kai_slab_cur + kai_slab_off;
    kai_slab_off += sz;
    return p;
}

static KaiVarSlot *kai_slots_alloc(int n) {
#ifndef KAI_NO_SLOT_POOL
    if (n >= 1 && n <= KAI_SLOT_POOL_MAXN && kai_slot_pool_n[n] > 0) {
        return kai_slot_pool[n][--kai_slot_pool_n[n]];
    }
#endif
    return (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
}

static void kai_slots_free(KaiVarSlot *slots, int n) {
    if (slots == NULL) return;
#ifndef KAI_NO_SLOT_POOL
    if (n >= 1 && n <= KAI_SLOT_POOL_MAXN && kai_slot_pool_n[n] < KAI_SLOT_POOL_CAP) {
        kai_slot_pool[n][kai_slot_pool_n[n]++] = slots;
        return;
    }
#endif
    free(slots);
}
#else
static KaiVarSlot *kai_slots_alloc(int n) {
    return (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
}
static void kai_slots_free(KaiVarSlot *slots, int n) { (void) n; free(slots); }
#endif

static KaiValue *kai_alloc(KaiTag tag) {
#endif
    KAI_PROF_ENTER();
#ifdef KAI_CELL_POOL_ACTIVE
    KaiValue *v;
    if (kai_cell_pool_n > 0) {
        v = kai_cell_pool[--kai_cell_pool_n];
        /* Zero the struct so callers see calloc-equivalent state (the
         * union, slots ptr, etc. must start clean). */
        memset(v, 0, sizeof(KaiValue));
    } else {
        kai_heap_charge(sizeof(KaiValue));
        v = (KaiValue *) calloc(1, sizeof(KaiValue));
    }
#else
    kai_heap_charge(sizeof(KaiValue));
    KaiValue *v = (KaiValue *) calloc(1, sizeof(KaiValue));
#endif
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) tag;
    /* trace */
    kai_rc_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    if ((int) tag >= 0 && (int) tag < 16) kai_rc_alloc_by_tag[(int) tag]++;
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) tag);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) tag);
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    v->scope_fn = kai_current_scope_fn;
    kai_leaksite_record_alloc(kai_current_scope_fn, (int32_t) tag);
    kai_leaksite_register_once();
#endif
    KAI_PROF_EXIT(alloc);
    return v;
}

#ifdef KAI_TRACE_RC
/* Macro shim: every call to kai_alloc(tag) inside a wrapper captures
 * the wrapper's caller (the real emit site) via
 * __builtin_return_address(0). Wrappers must be marked
 * KAI_RC_NOINLINE for this to point at the right frame. */
#define kai_alloc(tag) kai_alloc_traced((tag), __builtin_return_address(0))
#endif

/* Byte size of a variant block holding `n` inline slots. */
static inline size_t kai_var_block_size(int n) {
    /* Slots overlap the union at `offsetof(KaiValue, as)`, so the block is
     * the header up to `as` plus n slots — NOT sizeof(KaiValue) (which
     * counts the whole union) + n slots. A 5-slot node is offsetof(as)=8 +
     * 40 = 48 vs the old 80. Guard: at least sizeof(KaiValue) so a node
     * with few slots still has a valid base for the generic header reads.
     * `offsetof(KaiValue, as)` is the UB-free spelling — the old
     * `(char *)&((KaiValue *)1)->...var_slots[0] - (char *)1` trick formed a
     * member access on a misaligned bogus pointer that -fsanitize=undefined
     * flags on every variant alloc under the ASAN tier. */
    size_t base = offsetof(KaiValue, as);
    size_t need = base + (size_t) n * sizeof(KaiVarSlot);
    return need < sizeof(KaiValue) ? sizeof(KaiValue) : need;
}

/* FAM lane: allocate a KAI_VARIANT block with `n` payload slots stored
 * inline (one contiguous allocation). Mirrors kai_alloc's bookkeeping
 * but draws from / returns to the per-arity block pool instead of the
 * size-uniform cell pool, and never touches the separate slot pool.
 * The trace epilogue is shared with kai_alloc via the same counters. */
#ifdef KAI_TRACE_RC
static KaiValue *kai_alloc_var_traced(int n, void *site) {
#else
static KaiValue *kai_alloc_var(int n) {
#endif
    KAI_PROF_ENTER();
    KaiValue *v;
    size_t bsz = kai_var_block_size(n);
#ifdef KAI_CELL_POOL_ACTIVE
    if (n >= 0 && n <= KAI_VAR_BLOCK_POOL_MAXN && kai_var_block_pool_n[n] > 0) {
        v = kai_var_block_pool[n][--kai_var_block_pool_n[n]];
        memset(v, 0, bsz);
    } else {
        v = (KaiValue *) kai_slab_alloc(bsz);
        memset(v, 0, bsz);                 /* slab is uninit; this is the calloc-equivalent */
    }
#else
    /* No cell pool (KAI_TRACE_RC / KAI_PROFILE_RC / KAI_NO_CELL_POOL):
     * the slab allocator lives inside the cell-pool block, so fall back
     * to a plain libc allocation. kai_var_block_free's matching #else
     * calls free() on it. */
    kai_heap_charge(bsz);
    v = (KaiValue *) calloc(1, bsz);
#endif
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) KAI_VARIANT;
    kai_rc_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    kai_rc_alloc_by_tag[(int) KAI_VARIANT]++;
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) KAI_VARIANT);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) KAI_VARIANT);
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    v->scope_fn = kai_current_scope_fn;
    kai_leaksite_record_alloc(kai_current_scope_fn, (int32_t) KAI_VARIANT);
    kai_leaksite_register_once();
#endif
    KAI_PROF_EXIT(alloc);
    return v;
}
#ifdef KAI_TRACE_RC
#define kai_alloc_var(n) kai_alloc_var_traced((n), __builtin_return_address(0))
#endif

/* No-zero variant block allocator. Identical to kai_alloc_var except it
 * skips the calloc/memset zero-init: the caller (kai_variant_u_fast)
 * writes all n payload slots immediately after, so zeroing them is dead
 * work — measured 22M instructions (6%) of the rb-tree bench sat in
 * _int_malloc+calloc, the zero pass over 6.96M × 48 B nodes. malloc
 * leaves the slots indeterminate, which is sound ONLY because every slot
 * is overwritten before any read; the drop walker never sees an
 * unwritten slot. This is Koka's kk_alloc (raw malloc, no zero) — the
 * cell is fully initialised by the constructor, not by the allocator.
 *
 * The header fields the generic runtime reads (rc, tag, var_n_args,
 * variant_tag) are all written here or by the caller, so none rely on
 * the zero. Under tracing the alloc_site / scope_fn fields are written
 * explicitly too. */
#ifdef KAI_TRACE_RC
static KaiValue *kai_alloc_var_nz_traced(int n, void *site) {
#else
static KaiValue *kai_alloc_var_nz(int n) {
#endif
    KAI_PROF_ENTER();
    KaiValue *v;
#ifdef KAI_CELL_POOL_ACTIVE
    if (n >= 0 && n <= KAI_VAR_BLOCK_POOL_MAXN && kai_var_block_pool_n[n] > 0) {
        v = kai_var_block_pool[n][--kai_var_block_pool_n[n]];
        /* pool block: no memset — caller overwrites every slot */
    } else {
        v = (KaiValue *) kai_slab_alloc(kai_var_block_size(n));
    }
#else
    /* No cell pool: plain libc malloc (no-zero — caller fills slots);
     * kai_var_block_free's #else frees it. */
    v = (KaiValue *) kai_heap_malloc(kai_var_block_size(n));
#endif
    if (!v) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    v->rc = 1;
    v->tag = (uint8_t) KAI_VARIANT;
    kai_rc_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    kai_rc_alloc_by_tag[(int) KAI_VARIANT]++;
#ifdef KAI_TRACE_RC
    v->alloc_site = site;
    kai_rc_site_record_alloc(site, (int32_t) KAI_VARIANT);
    kai_rc_site_register_once();
    kai_rc_history_log(v, /* op=alloc */ 0, (int32_t) KAI_VARIANT);
#endif
#ifdef KAI_TRACE_RC_LEAKSITE
    v->scope_fn = kai_current_scope_fn;
    kai_leaksite_record_alloc(kai_current_scope_fn, (int32_t) KAI_VARIANT);
    kai_leaksite_register_once();
#endif
    KAI_PROF_EXIT(alloc);
    return v;
}
#ifdef KAI_TRACE_RC
#define kai_alloc_var_nz(n) kai_alloc_var_nz_traced((n), __builtin_return_address(0))
#endif

/* FAM lane: return a variant block (header + inline slots) to its
 * per-arity free-list pool. Used by kai_free_value's VARIANT case in
 * place of (kai_slots_free + cell-pool return).
 *
 * Slab-only invariant: every block lives inside a malloc'd slab
 * (kai_slab_alloc), so it is NOT a standalone malloc'd address — calling
 * libc free() on it is UB/corruption. So there is NO free(v) here. A
 * block that cannot rejoin the pool (pool full, or arity > MAXN) is
 * simply DROPPED: it stays in its slab and is reclaimed wholesale by
 * kai_slab_teardown at exit. Not an observable leak (ASAN: reachable via
 * the slab list until teardown). If a free() ever reaches here on a slab
 * pointer, ASAN fires "free on non-malloc'd address" at once. */
static void kai_var_block_free(KaiValue *v, int n) {
    (void) v;
#ifdef KAI_CELL_POOL_ACTIVE
    if (n >= 0 && n <= KAI_VAR_BLOCK_POOL_MAXN
        && kai_var_block_pool_n[n] < KAI_VAR_BLOCK_POOL_CAP) {
        kai_var_block_pool[n][kai_var_block_pool_n[n]++] = v;
        return;
    }
    /* spill: drop into the slab; teardown reclaims it at exit. */
#else
    /* No cell pool: blocks come from plain malloc/calloc (kai_alloc_var
     * #else), so free them individually — no slab to reclaim them, and
     * dropping here would leak (visible under the KAI_TRACE_RC tier). */
    (void) n;
    free(v);
#endif
}

/* m5 #7: constant pool for nullary primitives.
 *
 * `kai_unit()`, `kai_bool(true)`, `kai_bool(false)`, `kai_nil()` are
 * the four nullary constructors that dominated the per-tag alloc
 * breakdown on kaic2 self-compile (~78% combined). Returning shared
 * static singletons collapses every call to those factories into a
 * pointer to .data, eliminating the calloc and the four
 * `kai_rc_alloc_total` / `kai_rc_alloc_by_tag[]` increments per call.
 *
 * Singletons carry `rc = INT32_MAX` as a saturation sentinel.
 * `kai_incref` / `kai_decref` short-circuit when they see the
 * sentinel, so RC bookkeeping skips the singletons entirely:
 *   - incref leaves rc as-is (no overflow toward zero).
 *   - decref never triggers `kai_free_value` on a static, which
 *     would otherwise call `free()` on .data and crash.
 * The sentinel costs one extra `int` compare in the hot RC path;
 * the saved alloc/free traffic dominates by orders of magnitude.
 *
 * Selfhost stays byte-identical because the emitted text is
 * unchanged — only the runtime semantics shift.
 */
static KaiValue kai_singleton_unit  = { .rc = INT32_MAX, .tag = KAI_UNIT, .as = { .b = 0 } };
static KaiValue kai_singleton_true  = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 1 } };
static KaiValue kai_singleton_false = { .rc = INT32_MAX, .tag = KAI_BOOL, .as = { .b = 0 } };
static KaiValue kai_singleton_nil   = { .rc = INT32_MAX, .tag = KAI_NIL,  .as = { .b = 0 } };

/* issue #120 — opt-in Perceus regions: bump-arena primitive (P0).
 *
 * A `region { ... }` block bump-allocates every value constructed
 * inside it into an arena and frees the whole arena in one shot when
 * the brace closes — no per-value RC traffic, no per-value free walk.
 * This pays where allocation is tight LIFO scratch (lexer / parser /
 * formatter buffers): issue #120.
 *
 * KEY TRICK (asu + linus, docs/issue-120-regions-design.md): an arena
 * value carries `rc = INT32_MAX`, the EXISTING saturation sentinel the
 * runtime already short-circuits in kai_incref / kai_decref /
 * kai_check_unique. So to the RC machinery an arena value is
 * indistinguishable from a singleton:
 *   - decref is a no-op (kai_decref returns early on INT32_MAX) — FREE.
 *   - reuse-in-place auto-disables (kai_check_unique returns 0 for
 *     INT32_MAX) — FREE, no Perceus pass change.
 * No new tag, no new branch in the hot RC path, no struct growth.
 *
 * BOOKKEEPING LANDMINE: because kai_arena_free reclaims in bulk
 * WITHOUT walking values, kai_rc_live_now is never decremented per
 * value. Left alone, KAI_TRACE_RC leaked-vs-iterations gates would
 * report false leaks. Fix: a SEPARATE per-arena live count
 * (`n_live`) is subtracted from kai_rc_live_now on free, and the
 * dedicated kai_arena_alloc_total / kai_arena_free_total counters are
 * surfaced in kai_rc_report() so a wrong-codegen silent leak (a
 * non-region value mistakenly arena-allocated — invisible to ASAN
 * because nothing is freed) shows up as alloc/free divergence.
 *
 * PORTABILITY: malloc-backed chunks, NOT mmap — stage0 must build on
 * any ANSI cc with zero deps (CLAUDE.md "no deps in stage0"). The
 * arena stack is a plain global, not __thread: kaikai fibers each run
 * to a suspension point on the OS thread that dispatched them, so a
 * region never spans a fiber switch in v1. A per-fiber arena stack is
 * the natural follow-up once regions are allowed to straddle await
 * (see docs/lane-experience-issue-120.md). */

#define KAI_ARENA_CHUNK_BYTES (64 * 1024)
#define KAI_ARENA_ALIGN       16

typedef struct KaiArenaChunk {
    struct KaiArenaChunk *next;   /* grow-only singly-linked list */
    size_t                used;   /* bytes consumed in `data` */
    size_t                cap;    /* usable bytes in `data` */
    unsigned char        *data;   /* malloc-backed payload */
} KaiArenaChunk;

typedef struct KaiArena {
    KaiArenaChunk *head;          /* current (most-recently-grown) chunk */
    int64_t        n_live;        /* KaiValue headers stamped into this arena */
} KaiArena;

static size_t kai_arena_align_up(size_t n) {
    return (n + (KAI_ARENA_ALIGN - 1)) & ~((size_t) (KAI_ARENA_ALIGN - 1));
}

static KaiArenaChunk *kai_arena_chunk_new(size_t need) {
    size_t cap = KAI_ARENA_CHUNK_BYTES;
    if (need > cap) cap = need;               /* oversized single object */
    kai_heap_charge(sizeof(KaiArenaChunk) + cap);
    KaiArenaChunk *c = (KaiArenaChunk *) malloc(sizeof(KaiArenaChunk));
    if (!c) { fprintf(stderr, "kai: out of memory (arena chunk)\n"); exit(1); }
    c->data = (unsigned char *) malloc(cap);
    if (!c->data) { fprintf(stderr, "kai: out of memory (arena data)\n"); exit(1); }
    c->next = NULL;
    c->used = 0;
    c->cap  = cap;
    return c;
}

static void kai_arena_init(KaiArena *a) {
    a->head   = NULL;
    a->n_live = 0;
}

/* Raw aligned bump for interior storage (record `fields`, variant
 * `slots`, array `items`, KAI_STR `bytes`) so an aggregate built in a
 * region keeps ALL of its memory inside the arena — otherwise the
 * interior arrays would dangle on bulk free. Returns uninitialised
 * memory; the caller writes it. Not refcounted, not a KaiValue. */
static void *kai_arena_raw(KaiArena *a, size_t nbytes) {
    size_t need = kai_arena_align_up(nbytes ? nbytes : 1);
    if (!a->head || a->head->used + need > a->head->cap) {
        KaiArenaChunk *c = kai_arena_chunk_new(need);
        c->next = a->head;
        a->head = c;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += need;
    return p;
}

/* Bump a KaiValue header into the arena, stamped with the immortal
 * sentinel so RC skips it. Mirrors kai_alloc's header init (rc, tag,
 * zeroed union) but never calls calloc/free and never touches the
 * per-value RC counters — only the dedicated arena counters. Live-now
 * is bumped so live_peak stays honest mid-region; kai_arena_free
 * subtracts the whole batch back. */
static KaiValue *kai_arena_alloc(KaiArena *a, KaiTag tag) {
    KaiValue *v = (KaiValue *) kai_arena_raw(a, sizeof(KaiValue));
    memset(v, 0, sizeof(KaiValue));
    v->rc  = INT32_MAX;                 /* immortal sentinel: decref no-op */
    v->tag = (uint8_t) tag;
    a->n_live++;
    kai_arena_alloc_total++;
    kai_rc_live_now++;
    if (kai_rc_live_now > kai_rc_live_peak) kai_rc_live_peak = kai_rc_live_now;
    return v;
}

/* Bulk reclaim: free every chunk WITHOUT walking values (the whole
 * point — no per-value free, no per-value decref). Balance the trace
 * ledger by subtracting this arena's live count from kai_rc_live_now
 * and crediting kai_arena_free_total, then reset so the arena can be
 * reused. */
static void kai_arena_free(KaiArena *a) {
    KaiArenaChunk *c = a->head;
    while (c) {
        KaiArenaChunk *next = c->next;
        free(c->data);
        free(c);
        c = next;
    }
    kai_arena_free_total += a->n_live;
    kai_rc_live_now      -= a->n_live;
    a->head   = NULL;
    a->n_live = 0;
}

/* Lexical region stack. `region { ... }` pushes a fresh arena on
 * entry and pops+frees it on exit; constructors inside the block call
 * kai_arena_alloc on the current top. Nesting is bounded by lexical
 * block depth — 64 is far beyond any realistic region nesting and
 * keeps the stack a flat global with no allocation of its own. */
#define KAI_ARENA_STACK_MAX 64
static KaiArena kai_arena_stack[KAI_ARENA_STACK_MAX];
static int      kai_arena_sp = 0;

static KaiArena *kai_arena_push(void) {
    if (kai_arena_sp >= KAI_ARENA_STACK_MAX) {
        fprintf(stderr, "kai: region nesting exceeds %d\n", KAI_ARENA_STACK_MAX);
        exit(1);
    }
    KaiArena *a = &kai_arena_stack[kai_arena_sp++];
    kai_arena_init(a);
    return a;
}

static KaiArena *kai_arena_current(void) {
    if (kai_arena_sp == 0) return NULL;     /* not inside a region */
    return &kai_arena_stack[kai_arena_sp - 1];
}

static void kai_arena_pop(void) {
    if (kai_arena_sp == 0) return;
    kai_arena_free(&kai_arena_stack[--kai_arena_sp]);
}

/* Increment a reference. Pure fast path — no free, no out-of-line case
 * — so the whole body is inlined into the caller. Previously a non-inline
 * `static KaiValue *` (a real call at every dup, the symmetric cost to
 * the old non-inline kai_decref). KAI_PROF_ENTER/EXIT dropped: they
 * bracketed a single increment and blocked the inline. Under tracing the
 * counters still fire. Koka's kk_block_dup is likewise inline. */
static inline KaiValue *kai_incref(KaiValue *v) {
    if (kai_is_value(v) || !v || v->rc == INT32_MAX) {
#ifdef KAI_TRACE_RC
        if (v && !kai_is_value(v)) kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
        return v;
    }
    v->rc++;
    /* #812 — counter ALWAYS compiled (parallels kai_rc_alloc_total),
     * reported only under the KAI_TRACE_RC env var. Behind `#ifdef
     * KAI_TRACE_RC` it stayed 0 in every `kai build` binary (the wrapper
     * does not pass -DKAI_TRACE_RC), so each "RC balanced" gate that read
     * incref_total passed vacuously on 0 == 0. */
    kai_rc_incref_total++;
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=incref */ 1, v->tag);
#endif
    return v;
}
static void       kai_decref(KaiValue *v);

/* m8 #1/#3: KaiFiber definitions sit here (before kai_free_value)
 * because KAI_FIBER values own their KaiFiber struct and the free
 * path needs the full layout. The handler-stack runtime
 * (KaiEvidence + push/pop/lookup) lives further down; KaiFiber only
 * holds a `KaiEvidence *`, so a forward declaration is enough. The
 * Spawn default handlers (kai_default_spawn_*) come later in the
 * file too — they reach the struct through this declaration and use
 * kai_apply (defined further down) to invoke spawned thunks. */
typedef struct KaiEvidence KaiEvidence;

/* m8 #3 + m8.x: fiber lifecycle states. The pre-v0.4.0 runtime ran
 * `spawn` synchronously and only ever needed NEW/DONE; the m8.x
 * cooperative scheduler (landed v0.4.0) adds READY (enqueued in the
 * run queue), RUNNING (currently dispatched), and PARKED (blocked
 * on await / receive / send). Spec: docs/fibers-impl.md §*Fiber
 * state machine*. The numeric values are not stable across versions
 * — no ABI commitment yet (CLAUDE.md "Backward compatibility — not
 * promised until post-MVP"). */
typedef enum {
    KAI_FIBER_NEW       = 0,
    KAI_FIBER_READY     = 1,  /* m8.x */
    KAI_FIBER_RUNNING   = 2,  /* m8.x */
    KAI_FIBER_PARKED    = 3,  /* m8.x */
    KAI_FIBER_DONE      = 4,
    KAI_FIBER_CANCELLED = 5
} KaiFiberState;

typedef struct KaiFiber   KaiFiber;
typedef struct KaiNursery KaiNursery;     /* issue #959 — defined below */
typedef struct KaiLinkNode KaiLinkNode;  /* Phase 5 — defined below */
typedef struct KaiMonitorNode KaiMonitorNode;  /* Tier 2 Monitor — defined below */

struct KaiFiber {
    KaiEvidence    *evidence_top;
    int             cancel_requested;  /* Spawn.cancel(target) sets this (#4) */
    int             cancel_delivered;  /* Cancel.raise() injected once (#4)   */
    KaiFiber       *sched_next;        /* intrusive ready-queue link          */
    KaiFiber       *parent;            /* spawning fiber, NULL for the root   */
    KaiFiberState   state;
    KaiValue       *thunk;             /* held alive while the fiber runs     */
    KaiValue       *result;            /* set on DONE; what await returns     */
    /* m8.x cooperative scheduler additions. Spec: docs/fibers-impl.md
     * §*Scheduler*. main_fiber leaves ctx zero-initialised (filled in
     * by getcontext on first dispatch); spawned fibers fill ctx via
     * makecontext + a heap-allocated stack. */
    ucontext_t      ctx;               /* swapcontext target              */
    void           *stack_base;        /* heap-allocated; freed on RC=0   */
    size_t          stack_size;        /* set per fiber (env-configurable) */
    KaiFiber       *awaiters_head;     /* per-fiber awaiter chain head    */
    KaiFiber       *awaiters_next;     /* link into another fiber's chain */
    /* m8.x Phase 3 — Cancel delivery at yield points. The trampoline
     * sets up cancel_pad with setjmp before running the body; the
     * yield-point hook in kai_evidence_lookup* longjmps here when the
     * fiber's cancel_requested flag fires, unwinding the body to the
     * trampoline's cancel branch (state=CANCELLED). cancel_pad_set
     * gates the longjmp so it's only attempted while the pad is live
     * — main_fiber never runs through the trampoline, so its
     * cancel_pad stays unset and Cancel.raise() in main falls back to
     * exit(0) (the m8 v1 behaviour for unhandled root cancellation). */
    jmp_buf         cancel_pad;
    int             cancel_pad_set;
    /* Phase 5 — intrusive list of linked peer fibers. Walked at
     * trampoline termination (DONE or CANCELLED branches) to set
     * cancel_requested on each peer. Owned by the fiber; nodes are
     * freed during the propagation walk and as a safety net in
     * kai_free_value's KAI_FIBER branch. */
    KaiLinkNode    *linked_head;
    /* m8 bug #12 (per-fiber dispatch state). Points at the evidence
     * node whose clause body is currently on this fiber's stack, or
     * NULL otherwise. `kai_evidence_lookup_node` skips it so a
     * `Eff.op(...)` invoked from inside the clause resolves to the
     * outer handler instead of recursing. Lives on the fiber (not
     * the node) because spawned fibers inherit the parent's
     * evidence_top — the same node is shared across fibers, so the
     * "in dispatch right now" state must be per-fiber. The op-call
     * site saves the previous value, sets this to its own node,
     * runs the clause, then restores. */
    KaiEvidence    *in_dispatch_node;
    /* R4 fix — back-pointer to the KaiValue wrapper that owns this
     * fiber struct. Set in kai_fiber_value when the wrapper is
     * allocated; the scheduler holds an incref on the wrapper from
     * spawn-enqueue until the trampoline's DONE/CANCELLED tail, which
     * `kai_decref`s `value`. Pairing the scheduler-side ref with the
     * caller-side ref makes `let _ = fiber_spawn(…)` (discarding the
     * Fiber value) safe: the wrapper stays alive while the struct is
     * still referenced from the run queue. NULL on `kai_main_fiber`,
     * which has no wrapper (it represents the OS thread). */
    KaiValue       *value;
    /* Tier 2 — trap-exit semantics. When 0 (default), a linked peer's
     * termination sets cancel_requested on this fiber (the v1 uniform
     * propagation). When 1, the propagation walk pushes a String into
     * this fiber's mailbox instead — "Normal" if the peer terminated
     * via DONE, "Crashed" if via CANCELLED — and leaves
     * cancel_requested untouched. Toggled by Spawn.set_trap_exit;
     * spec: docs/actors.md §*Supervision: links and monitors* /
     * *Trap-exit semantics*. The flag is per-fiber, not per-link, so
     * it must be set before the link that should respect it; later
     * toggles affect future propagations only. */
    int             trap_exit;
    /* Tier 2 — most-recently-allocated mailbox owned by this fiber.
     * Set by kai_mailbox_alloc[_bounded] and cleared by
     * kai_mailbox_free. Read by kai_link_propagate_terminate when
     * trap_exit=1 to find a delivery target for the Exit string.
     * v1 simplification: nested with_mailbox is not tracked — the
     * inner allocation overwrites and the inner free clears the
     * slot, leaving the outer mailbox unreachable to the trap-exit
     * walker until the inner scope exits. Demos do not nest.
     * Forward-declared as struct KaiMailbox * because the full
     * KaiMailbox typedef sits below KaiFiber in this header. */
    struct KaiMailbox *mailbox;
    /* Tier 2 — intrusive list of fibers monitoring this one. Each
     * Monitor.monitor(target_pid) call from an observer fiber
     * appends a node here on the *target* fiber. At trampoline
     * termination (DONE or CANCELLED) the walker pops every entry
     * and pushes the original target_pid value into the observer's
     * mailbox, leaving the observer's cancel_requested untouched
     * (monitors do not propagate faults — `docs/actors.md` §*Fault
     * propagation*). Owned by the target fiber; nodes are freed in
     * the propagation walk and as a safety net in
     * kai_free_value's KAI_FIBER branch. */
    KaiMonitorNode *monitor_head;
    /* Issue #611 — Phase R1 reactor metadata. A parked fiber lives
     * on exactly one reactor list at a time (timer wheel, pid waiter
     * map, or file-pool waiter list), so a single intrusive link slot
     * is sufficient. The companion data members carry the wakeup
     * payload that the parking op needs to read on resume:
     *   reactor_deadline_ns — monotonic deadline for timer-wheel parks.
     *   reactor_wait_pid    — pid the fiber is waiting on (Process.wait).
     *   reactor_wait_status — waitpid status filled by the SIGCHLD drain.
     *   reactor_data        — generic pointer slot used by the file
     *                         thread-pool offload to publish the
     *                         completed `KaiValue *` result before
     *                         waking the parked fiber.
     * The slots are read once on resume; nothing else in the runtime
     * touches them. Zero is a valid "not parked here" sentinel for
     * all four. */
    KaiFiber       *reactor_next;
    uint64_t        reactor_deadline_ns;
    int             reactor_wait_pid;
    int             reactor_wait_status;
    void           *reactor_data;
    /* Issue #959 — structured-concurrency scope. `nursery_top` is the
     * innermost open nursery on THIS fiber (a per-fiber stack so a
     * spawned child opening its own nursery does not collide with the
     * parent's; nesting composes Trio-style). `Spawn.spawn` registers
     * the new child on `kai_active_fiber->nursery_top`'s children list
     * via `scope_sibling_next`. `nursery_exit` joins every child on
     * that list before returning, cancelling the rest and re-raising
     * on the first child that terminated CANCELLED. NULL when no
     * nursery is open (bare spawns then have no scope to join). */
    struct KaiNursery *nursery_top;
    KaiFiber          *scope_sibling_next;
};

/* Issue #959 — one open structured-concurrency scope. Children spawned
 * while this scope is the active fiber's `nursery_top` are pushed on
 * `children_head` (intrusive via the child's `scope_sibling_next`).
 * `parent` chains to the enclosing nursery so a nested `nursery { }`
 * restores it on exit. The scope holds one extra ref on each child's
 * wrapper (taken at registration, released at join) so a discarded
 * `Fiber[T]` handle cannot free the struct before the scope joins it. */
struct KaiNursery {
    KaiFiber   *children_head;
    KaiNursery *parent;
};

/* m7a #5 + m8.x: kai_main_fiber starts as the OS-thread context,
 * representing the dispatch loop. Its ctx is filled lazily on first
 * yield (getcontext at the moment we suspend the dispatch loop into
 * a fiber). The active-fiber pointer (kai_active_fiber) tracks
 * whoever is currently executing; kai_current_fiber returns it.
 * Spec: docs/fibers-impl.md §*Dispatch loop*. */
static KaiFiber kai_main_fiber = {
    NULL,                /* evidence_top */
    0, 0,                /* cancel_requested, cancel_delivered */
    NULL, NULL,          /* sched_next, parent */
    KAI_FIBER_RUNNING,   /* state — main starts running on the OS thread */
    NULL, NULL,          /* thunk, result */
    {0},                 /* ctx — getcontext fills it on first swap */
    NULL, 0,             /* stack_base, stack_size — main uses the OS stack */
    NULL, NULL,          /* awaiters_head, awaiters_next */
    {0}, 0,              /* cancel_pad, cancel_pad_set — main has no pad */
    NULL,                /* linked_head */
    NULL,                /* in_dispatch_node */
    NULL,                /* value — main has no wrapper */
    0,                   /* trap_exit — main starts opted out */
    NULL,                /* mailbox — set by with_mailbox if main uses one */
    NULL,                /* monitor_head — Monitor.monitor(...) appends here */
    NULL, 0, 0, 0, NULL, /* reactor_next, _deadline_ns, _wait_pid, _wait_status, _data */
    NULL, NULL           /* nursery_top, scope_sibling_next — issue #959 */
};

static KaiFiber *kai_active_fiber = &kai_main_fiber;

static KaiFiber *kai_current_fiber(void) {
    return kai_active_fiber;
}

/* m8 #1 + m8.x: ready queue (intrusive singly-linked, head/tail).
 * Fibers go on the queue when spawned (NEW→READY) or unparked
 * (PARKED→READY); off the queue when dispatched (READY→RUNNING).
 * The dispatch loop drains it; deadlock detection panics when the
 * queue is empty *and* parked fibers exist with no wakeup path. */
static KaiFiber *kai_ready_head = NULL;
static KaiFiber *kai_ready_tail = NULL;
static int       kai_parked_count = 0;  /* deadlock detection */

/* R4 fix — single-slot pending-free for fiber structs whose wrappers
 * went to RC=0 while the fiber itself was still the current fiber
 * (the trampoline tail's `kai_decref(self->value)` is the producer).
 * Drained at every entry point that follows a context switch (top of
 * trampoline, post-swapcontext in yield/park) so the freed stack is
 * never the one we are running on. */
static KaiFiber *kai_pending_free = NULL;

/* Forward decl — defined alongside the m8.x fiber stack allocator
 * later in the file, but used here by the free path (munmap needs the
 * stack_size + page_size total). */
static size_t kai_page_size(void);

/* Linux's <sys/mman.h> exposes MAP_ANONYMOUS; older BSD-style headers
 * (and the legacy macOS spelling) use MAP_ANON. Define whichever the
 * platform omits in terms of the other so the fiber stack mmap call
 * compiles on both. */
#if !defined(MAP_ANON) && defined(MAP_ANONYMOUS)
#  define MAP_ANON MAP_ANONYMOUS
#endif

static void kai_drain_pending_free(void) {
    KaiFiber *f = kai_pending_free;
    if (!f) return;
    kai_pending_free = NULL;
    /* thunk / result / linked_head were handled at wrapper-free time;
     * only stack + struct remain. The stack is an mmap region of
     * size stack_size + one guard page; pair the call with munmap
     * (free would corrupt the heap). */
    if (f->stack_base) {
        munmap(f->stack_base, f->stack_size + kai_page_size());
    }
    free(f);
}

/* m8 #3: wrap a heap-allocated KaiFiber in an opaque KAI_FIBER
 * value. The KaiFiber struct's lifetime is tied to the value's RC
 * (kai_free_value frees both together). The struct's `value`
 * back-pointer lets the scheduler retain its own incref on the
 * wrapper from spawn-enqueue until the trampoline tail (R4 fix). */
static KAI_RC_NOINLINE KaiValue *kai_fiber_value(KaiFiber *f) {
    KaiValue *v = kai_alloc(KAI_FIBER);
    v->as.fib = f;
    f->value  = v;
    return v;
}

/* m8 #7 + m8.x: mailbox runtime. A KaiMailbox is a singly-linked
 * list of heap-allocated KaiValue messages (head = next-to-pop,
 * tail = next-to-enqueue). Send pushes at the tail; receive pops
 * the head. Receive on an empty mailbox parks the caller on
 * recv_waiter (`kai_mailbox_pop`) and yields to the cooperative
 * scheduler; the next push wakes the head waiter (FIFO). All four
 * overflow policies (Unbounded / DropOldest / DropNewest /
 * BlockSender) reach the runtime: DropOldest / DropNewest mutate
 * the buffer in place; BlockSender parks the sender on
 * send_waiter when full and resumes when a receiver pops a slot. */
typedef struct KaiMboxNode KaiMboxNode;
struct KaiMboxNode {
    KaiValue    *msg;
    KaiMboxNode *next;
};

/* m8 #8 + m8.x: mailbox overflow policy codes (matched in
 * stdlib/actor.kai by the MailboxPolicy enum). 0 = Unbounded,
 * 1 = Bounded+DropOldest, 2 = Bounded+DropNewest,
 * 3 = Bounded+BlockSender. All four policies are implemented:
 * `kai_mailbox_alloc_bounded` accepts every code; `kai_mailbox_push`
 * dispatches on the policy and parks the sender on `send_waiter`
 * for BlockSender via the cooperative scheduler. */
#define KAI_OVERFLOW_UNBOUNDED    0
#define KAI_OVERFLOW_DROP_OLDEST  1
#define KAI_OVERFLOW_DROP_NEWEST  2
#define KAI_OVERFLOW_BLOCK_SENDER 3

typedef struct KaiMailbox KaiMailbox;
struct KaiMailbox {
    KaiMboxNode *head;
    KaiMboxNode *tail;
    int          len;
    int          cap;       /* m8 #8: 0 = unbounded; >0 = bounded */
    int          overflow;  /* m8 #8: KAI_OVERFLOW_* code */
    /* Phase 4 — blocking primitives. Waiter queues for fibers
     * parked on empty receive (head==NULL) or on full BlockSender
     * push (len>=cap). Linked through each fiber's awaiters_next
     * field — a fiber is in exactly one waiter chain at a time
     * (await chain, receiver chain, or sender chain), so the
     * single field is sufficient. */
    KaiFiber    *recv_waiter_head;
    KaiFiber    *recv_waiter_tail;
    KaiFiber    *send_waiter_head;
    KaiFiber    *send_waiter_tail;
    /* Phase 5 — owner fiber for the Link/Monitor runtime.
     * Set to kai_current_fiber() at allocation. Link.link(pid)
     * resolves pid->mb->owner_fiber to find the target fiber to
     * link to. v1 maps each mailbox to exactly one owning fiber
     * (the one that called mailbox_alloc); spawn_actor (when it
     * lands in m8.x #6) will set owner_fiber to the spawned
     * fiber instead. */
    KaiFiber    *owner_fiber;
};

/* Phase 5 — intrusive linked-peer chain on KaiFiber. A bidirectional
 * link between two fibers consists of one KaiLinkNode in each
 * fiber's linked_head chain pointing at the other peer. On fiber
 * termination (DONE or CANCELLED in the trampoline), the chain is
 * walked and each peer's cancel_requested flag is set. The doc
 * spec distinguishes Normal vs Crashed termination for link
 * propagation; v1 propagates on both DONE and CANCELLED (BEAM
 * trap-exit semantics is queued for post-MVP).
 *
 * The forward typedef is at the KaiFiber declaration above so
 * KaiFiber can hold a `KaiLinkNode *linked_head`. */
struct KaiLinkNode {
    KaiFiber    *peer;
    KaiLinkNode *next;
};

/* Tier 2 Monitor — intrusive node sitting on the *target* fiber's
 * monitor_head chain. observer = the fiber that called
 * Monitor.monitor(target_pid); target_pid = the same KaiValue *
 * the user passed to monitor(...) (the runtime owns one ref via
 * kai_incref so the value survives until the target terminates).
 * On termination, the propagate walker pushes target_pid into
 * observer->mailbox and frees the node.
 *
 * The forward typedef is at the KaiFiber declaration above so
 * KaiFiber can hold a `KaiMonitorNode *monitor_head`. */
struct KaiMonitorNode {
    KaiFiber       *observer;
    KaiValue       *target_pid;
    KaiMonitorNode *next;
};

/* Phase 4 forward decls: mailbox push/pop park/wake the calling
 * fiber via the scheduler primitives, which are defined further
 * down (after the handler-stack runtime). The decls here let the
 * mailbox ops compile in their natural file location. */
static void kai_sched_park(void);
static void kai_sched_unpark(KaiFiber *target);
/* Issue #679: park sites in the reactor call this after wake to
 * observe a sibling-triggered cancel before retrying their syscall.
 * Body lives near the op-call lookup prologue at line 8868+. */
static void kai_check_cancel_yield_point(void);

/* Issue #611 — Phase R1 reactor forward decls. The default Clock /
 * File / Process handlers live above the reactor implementation
 * but call into it through the small API below to park their
 * fibers. Bodies sit alongside the scheduler primitives a few
 * thousand lines further down. */
typedef struct KaiFilepoolItem KaiFilepoolItem;
static void     kai_reactor_init(void);
static void     kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns);
static void     kai_reactor_park_pid(KaiFiber *f, int pid);
static KaiValue *kai_reactor_run_in_pool(KaiValue *(*work)(void *), void *arg);
static uint64_t kai_reactor_now_ns(void);
/* Timeout-receive dual-park: arm a deadline alongside the mailbox park
 * and disarm it if a message wins. Bodies sit with the timer wheel. */
static void     kai_reactor_timer_insert(KaiFiber *f);
static int      kai_reactor_timer_remove(KaiFiber *f);

/* Issue #620 — Phase R3 reactor forward decls. The default Stdin
 * handlers live ~1700 lines above the reactor implementation. They
 * call into the small park-stdin helper which encapsulates the
 * single-fiber slot, the parked-count bump, and the kai_sched_park
 * yield. The nonblock-once helper is also forward-declared. */
static int       kai_reactor_park_stdin(KaiFiber *f);
static void      kai_reactor_stdin_set_nonblocking(void);

/* Issue #630 — Phase R2 reactor forward decls. The six NetTcp
 * default handlers live ~1000 lines above the reactor; they call
 * these helpers to park on read/write readiness of a socket fd.
 * A fiber sits on at most one socket-direction list at a time
 * (a single op is either reading or writing, never both), so the
 * existing `reactor_next` intrusive slot is sufficient. The fd is
 * stashed in `reactor_wait_pid` (the slot is otherwise unused while
 * a fiber is parked on a socket — pids and sockets are mutually
 * exclusive park reasons). */
static void      kai_reactor_park_socket_read(KaiFiber *f, int fd);
static void      kai_reactor_park_socket_write(KaiFiber *f, int fd);
static void      kai_socket_set_nonblock(int fd);

/* Issue #671 — Phase R4: park `f` on the singleton signal waiter
 * slot and yield. Returns 0 on success, -1 if a second fiber is
 * already parked (concurrent `Signal.await()` is undefined — v1
 * panics at the call site with a clear diagnostic, mirroring the
 * R3 stdin-multiplex contract). The signo arrives on resume via
 * `f->reactor_wait_status`. */
static int       kai_reactor_park_signal(KaiFiber *f);

/* Issue #671 — Phase R4: async-signal-safe handler installed for
 * every subscribed signal. Forward-declared because it is used by
 * `kai_default_signal_on` (~line 6300) but its body lives down in
 * the reactor block (~line 7030, alongside kai_reactor_sigchld_handler). */
static void      kai_reactor_signal_handler(int sig);

static KaiMailbox *kai_mailbox_alloc(void) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    /* default policy: unbounded — matches m8 #7 behaviour. */
    mb->cap          = 0;
    mb->overflow     = KAI_OVERFLOW_UNBOUNDED;
    /* Phase 5: associate the mailbox with the allocating fiber. v1
     * actor surface (with_mailbox) is the only path to mailbox_alloc,
     * and the allocating fiber IS the actor; spawn_actor uses
     * kai_mailbox_alloc_unowned + kai_mailbox_assign_owner to point
     * at the spawned fiber instead of the parent. */
    mb->owner_fiber  = kai_current_fiber();
    /* Tier 2 trap-exit: stamp the mailbox onto its owner fiber so
     * kai_link_propagate_terminate can locate it without a global
     * registry. Nested allocs overwrite; kai_mailbox_free clears. */
    if (mb->owner_fiber) mb->owner_fiber->mailbox = mb;
    return mb;
}

/* Tier 2 spawn_actor — variant of kai_mailbox_alloc that does NOT
 * stamp owner_fiber. Used by stdlib's `spawn_actor` so the
 * mailbox's owner can be reassigned to the spawned fiber via
 * kai_mailbox_assign_owner before the spawned body runs. The
 * `mailbox` slot on the parent fiber is left untouched too —
 * spawn_actor's mailbox does not belong to the parent. */
static KaiMailbox *kai_mailbox_alloc_unowned(void) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    mb->cap         = 0;
    mb->overflow    = KAI_OVERFLOW_UNBOUNDED;
    mb->owner_fiber = NULL;  /* set later by kai_mailbox_assign_owner */
    return mb;
}

static KaiMailbox *kai_mailbox_alloc_bounded(int cap, int overflow) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    /* Phase 4: BlockSender now supported via the per-mailbox sender
     * waiter queue + cooperative parking on full push. */
    mb->cap          = cap;
    mb->overflow     = overflow;
    mb->owner_fiber  = kai_current_fiber();
    if (mb->owner_fiber) mb->owner_fiber->mailbox = mb;
    return mb;
}

/* Issue #763 spawn_actor_policy — bounded counterpart of
 * kai_mailbox_alloc_unowned. No owner stamp and the parent fiber's
 * `mailbox` slot stays untouched; ownership is wired to the spawned
 * fiber afterwards via kai_mailbox_assign_owner. Routing through
 * kai_mailbox_alloc_bounded instead would overwrite the parent's
 * `mailbox` slot, corrupting its monitor / link / trap-exit lookups
 * whenever the parent already owns a mailbox. */
static KaiMailbox *kai_mailbox_alloc_bounded_unowned(int cap, int overflow) {
    KaiMailbox *mb = (KaiMailbox *) calloc(1, sizeof(KaiMailbox));
    if (!mb) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    mb->cap         = cap;
    mb->overflow    = overflow;
    mb->owner_fiber = NULL;  /* set later by kai_mailbox_assign_owner */
    return mb;
}

/* Phase 4 helper: link a fiber into a waiter chain at the tail. */
static void kai_mailbox_waiter_enqueue(KaiFiber **head, KaiFiber **tail, KaiFiber *f) {
    f->awaiters_next = NULL;
    if (*tail) {
        (*tail)->awaiters_next = f;
    } else {
        *head = f;
    }
    *tail = f;
}

/* Phase 4 helper: pop the head waiter from a chain (FIFO wakeup). */
static KaiFiber *kai_mailbox_waiter_dequeue(KaiFiber **head, KaiFiber **tail) {
    KaiFiber *f = *head;
    if (!f) return NULL;
    *head = f->awaiters_next;
    if (!*head) *tail = NULL;
    f->awaiters_next = NULL;
    return f;
}

/* Splice `f` out of a waiter chain regardless of position. Returns 1
 * if found. The timeout-receive uses this to drop itself from the
 * recv-waiter chain when its deadline fires first, so a later send
 * does not unpark a fiber that already returned `None`. */
static int kai_mailbox_waiter_remove(KaiFiber **head, KaiFiber **tail, KaiFiber *f) {
    KaiFiber **link = head;
    KaiFiber  *prev = NULL;
    while (*link) {
        if (*link == f) {
            *link = f->awaiters_next;
            if (!*link) *tail = prev;
            f->awaiters_next = NULL;
            return 1;
        }
        prev = *link;
        link = &(*link)->awaiters_next;
    }
    return 0;
}

static void kai_mailbox_push(KaiMailbox *mb, KaiValue *msg) {
    /* m8 #8 + Phase 4: enforce policy on full. */
    if (mb->cap > 0 && mb->len >= mb->cap) {
        if (mb->overflow == KAI_OVERFLOW_DROP_NEWEST) {
            kai_decref(msg);
            return;
        } else if (mb->overflow == KAI_OVERFLOW_DROP_OLDEST) {
            /* Pop and discard the head; fall through to enqueue. */
            KaiMboxNode *old = mb->head;
            mb->head = old->next;
            if (!mb->head) { mb->tail = NULL; }
            kai_decref(old->msg);
            free(old);
            mb->len--;
        } else if (mb->overflow == KAI_OVERFLOW_BLOCK_SENDER) {
            /* Park sender until a receiver pops a slot. The cooperative
             * scheduler guarantees forward progress: the receiver that
             * eventually pops will wake one parked sender (FIFO). The
             * loop re-checks because between unpark and resume another
             * sender could have refilled the slot. */
            while (mb->len >= mb->cap) {
                kai_mailbox_waiter_enqueue(&mb->send_waiter_head,
                                            &mb->send_waiter_tail,
                                            kai_current_fiber());
                kai_sched_park();
            }
        }
    }
    KaiMboxNode *node = (KaiMboxNode *) calloc(1, sizeof(KaiMboxNode));
    if (!node) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    node->msg  = msg;
    node->next = NULL;
    if (mb->tail) { mb->tail->next = node; }
    else          { mb->head       = node; }
    mb->tail = node;
    mb->len++;
    /* Phase 4: wake one parked receiver if any. */
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->recv_waiter_head,
                                                    &mb->recv_waiter_tail);
    if (waiter) kai_sched_unpark(waiter);
}

static KaiValue *kai_mailbox_pop(KaiMailbox *mb) {
    /* Phase 4: park the calling fiber until a sender enqueues. The
     * loop handles the case where another receiver took the slot
     * between our unpark and resume (rare but possible if multiple
     * fibers race on the same mailbox). */
    while (!mb->head) {
        kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                    &mb->recv_waiter_tail,
                                    kai_current_fiber());
        kai_sched_park();
    }
    KaiMboxNode *node = mb->head;
    mb->head = node->next;
    if (!mb->head) { mb->tail = NULL; }
    KaiValue *msg = node->msg;
    free(node);
    mb->len--;
    /* Phase 4: wake one parked sender if any (only meaningful for
     * BlockSender; unbounded and Drop* mailboxes never park senders). */
    KaiFiber *waiter = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                                    &mb->send_waiter_tail);
    if (waiter) kai_sched_unpark(waiter);
    return msg;
}

/* Receive with a deadline: park the caller on BOTH the recv-waiter
 * chain and the timer wheel, woken by whichever fires first. Returns
 * the popped message, or NULL if `timeout_nanos` elapsed first.
 *
 * The discriminant on wake is recv-waiter membership: a send unparks
 * us by dequeuing us from the recv-waiter chain (we stay on the timer
 * wheel), while the timer drain unparks us off the wheel (we stay on
 * the recv-waiter chain). Whichever wakeup ran, we splice ourselves
 * out of the OTHER structure so a later event cannot touch a fiber
 * that already returned — the use-after-free trap this guards.
 *
 * A spurious message-wake (another receiver drained the slot first)
 * re-parks for the remaining time; an elapsed deadline returns NULL. */
static KaiValue *kai_mailbox_pop_timeout(KaiMailbox *mb, uint64_t timeout_nanos) {
    if (mb->head) {
        KaiMboxNode *node = mb->head;
        mb->head = node->next;
        if (!mb->head) { mb->tail = NULL; }
        KaiValue *msg = node->msg;
        free(node);
        mb->len--;
        KaiFiber *sw = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                                   &mb->send_waiter_tail);
        if (sw) kai_sched_unpark(sw);
        return msg;
    }
    uint64_t deadline = kai_reactor_now_ns() + timeout_nanos;
    for (;;) {
        if (kai_reactor_now_ns() >= deadline) return NULL;
        KaiFiber *me = kai_current_fiber();
        kai_mailbox_waiter_enqueue(&mb->recv_waiter_head,
                                   &mb->recv_waiter_tail, me);
        kai_reactor_park_timer(me, deadline);

        /* park_timer's drain already spliced us off the wheel on a
         * deadline wake. Disarm it on a message wake; in both cases
         * remove ourselves from the chain we are still linked into. */
        int still_waiting = kai_mailbox_waiter_remove(&mb->recv_waiter_head,
                                                      &mb->recv_waiter_tail, me);
        if (still_waiting) {
            /* Deadline fired: we were never dequeued by a send. */
            return NULL;
        }
        /* A send dequeued us; disarm the still-armed deadline timer. */
        kai_reactor_timer_remove(me);
        if (mb->head) {
            KaiMboxNode *node = mb->head;
            mb->head = node->next;
            if (!mb->head) { mb->tail = NULL; }
            KaiValue *msg = node->msg;
            free(node);
            mb->len--;
            KaiFiber *sw = kai_mailbox_waiter_dequeue(&mb->send_waiter_head,
                                                      &mb->send_waiter_tail);
            if (sw) kai_sched_unpark(sw);
            return msg;
        }
        /* Spurious: another receiver took the slot. Loop re-checks the
         * deadline and re-parks for whatever time remains. */
    }
}

static void kai_mailbox_free(KaiMailbox *mb) {
    if (!mb) return;
    /* Tier 2 trap-exit: drop the back-pointer if the owner fiber
     * still has us as its current mailbox. Nested with_mailbox: the
     * inner free clears the slot even though the outer mailbox is
     * still alive — accepted v1 limitation. */
    if (mb->owner_fiber && mb->owner_fiber->mailbox == mb) {
        mb->owner_fiber->mailbox = NULL;
    }
    KaiMboxNode *node = mb->head;
    while (node) {
        KaiMboxNode *next = node->next;
        kai_decref(node->msg);
        free(node);
        node = next;
    }
    free(mb);
}

/* m8 #7: wrap a borrowed mailbox pointer as a KAI_PID value. The
 * mailbox itself is owned by the with_mailbox / spawn_actor scope
 * that allocated it; Pid values are non-owning handles that just
 * carry the address. */
static KAI_RC_NOINLINE KaiValue *kai_pid_value(KaiMailbox *mb) {
    KaiValue *v = kai_alloc(KAI_PID);
    v->as.mb = mb;
    return v;
}

/* Issue #817 — the recycle-this-cell tail shared by kai_free_value's
 * non-cons cases and kai_free_cons_spine's per-cell reclaim. Runs the
 * trace/poison bookkeeping (#812 counters, #296 alloc-site credit,
 * history log, poison stamp) and returns the chunk to the pool (or
 * libc), then bumps the free counters. Must run exactly once per
 * reclaimed cell — double invocation is a double-free + double counter.
 * Reads `_kc->tag` / `_kc->var_n_args` BEFORE the poison stamp clobbers
 * them, so callers must not have poisoned the cell. */
#define KAI_RECYCLE_CELL(cell) do {                                          \
    KaiValue *_kc = (cell);                                                  \
    KAI_RC_RECYCLE_TRACE(_kc);                                               \
    KAI_RC_RECYCLE_POOL(_kc);                                                \
    kai_rc_free_total++;                                                     \
    kai_rc_live_now--;                                                       \
} while (0)

#ifdef KAI_TRACE_RC
#define KAI_RC_RECYCLE_TRACE(_kc) do {                                       \
    int32_t freed_tag = (_kc)->tag;                                          \
    if (freed_tag >= 0 && freed_tag < 16) kai_rc_free_by_tag[freed_tag]++;   \
    kai_rc_site_record_free((_kc)->alloc_site);                              \
    kai_rc_history_log((_kc), /* op=free */ 3, freed_tag);                   \
    KAI_RC_RECYCLE_LEAKSITE(_kc, freed_tag);                                 \
    { uint64_t *p64 = (uint64_t *) (_kc);                                    \
      size_t nq = sizeof(KaiValue) / sizeof(uint64_t);                       \
      for (size_t i = 0; i < nq; i++) p64[i] = KAI_RC_SENTINEL_U64; }        \
} while (0)
#ifdef KAI_TRACE_RC_LEAKSITE
#define KAI_RC_RECYCLE_LEAKSITE(_kc, ft) kai_leaksite_record_free((_kc)->scope_fn, (ft))
#else
#define KAI_RC_RECYCLE_LEAKSITE(_kc, ft) ((void) 0)
#endif
#else
#define KAI_RC_RECYCLE_TRACE(_kc) ((void) 0)
#endif

#ifdef KAI_CELL_POOL_ACTIVE
#define KAI_RC_RECYCLE_POOL(_kc) do {                                        \
    if ((_kc)->tag == (int32_t) KAI_VARIANT) {                              \
        kai_var_block_free((_kc), (_kc)->var_n_args);                        \
    } else if (kai_cell_pool_n < KAI_CELL_POOL_CAP) {                        \
        kai_cell_pool[kai_cell_pool_n++] = (_kc);                            \
    } else {                                                                 \
        free(_kc);                                                           \
    }                                                                        \
} while (0)
#else
#define KAI_RC_RECYCLE_POOL(_kc) free(_kc)
#endif

/* Issue #817 — free a cons cell's payload and walk a UNIQUE tail spine
 * iteratively, so a 40K-element list frees in O(1) stack instead of O(n)
 * recursion (the recursive `kai_decref(tail)` overflowed a 64 KiB fiber
 * stack once filter/map stopped leaking the spine). Precondition: `v` is
 * a KAI_CONS cell whose rc just hit 0 (we own its free). Each head — and
 * any shared (rc>1) or non-cons tail — goes through the ordinary
 * `kai_decref` so its counters, guards and cascade stay intact; only the
 * unique cons spine is consumed by the loop. Each cell is reclaimed via
 * KAI_RECYCLE_CELL, the same path the post-switch tail uses. */
static void kai_free_cons_spine(KaiValue *v) {
    for (;;) {
        KaiValue *head = v->as.cons.head;
        KaiValue *tail = v->as.cons.tail;   /* capture BEFORE recycle poisons v */
        kai_decref(head);                   /* O(1) cascade for str/record; counters intact */
        KAI_RECYCLE_CELL(v);                /* trace+poison+recycle+free_total++/live_now-- */
        /* Continue only for a real, unique cons cell. A nil/singleton
         * (kai_is_value or rc==INT32_MAX), a non-cons, or a shared
         * (rc!=1) tail hands off to kai_decref for its own free path. */
        if (kai_is_value(tail) || !tail ||
            tail->rc == INT32_MAX ||
            tail->tag != (int32_t) KAI_CONS ||
            tail->rc != 1) {
            kai_decref(tail);               /* counters + cascade for the boundary case */
            return;
        }
        /* We are the unique owner of `tail`; consume it as `kai_decref`
         * would (counter + trace history) but without the recursive call.
         * (The FIRST cell's decref counter was charged by the `kai_decref`
         * that called us; the loop charges the counter for each tail cell
         * it consumes here, so the total stays byte-identical to the
         * recursive version.) */
        kai_rc_decref_total++;
#ifdef KAI_TRACE_RC
        kai_rc_history_log(tail, /* op=decref */ 2, tail->tag);
#endif
        tail->rc = 0;
        v = tail;                           /* loop, no stack growth */
    }
}

static void kai_free_value(KaiValue *v) {
    KAI_PROF_ENTER();
    switch ((KaiTag) v->tag) {
        case KAI_STR:
            free(v->as.s.bytes);
            break;
        case KAI_CONS:
            /* Issue #817 — iterative spine free; reclaims v and the whole
             * unique tail spine, then returns (it already ran the recycle
             * + counters for v, so we must NOT fall to the post-switch
             * tail — that would double-free v). */
            kai_free_cons_spine(v);
            KAI_PROF_EXIT(free);
            return;
        case KAI_RECORD:
            for (int i = 0; i < v->as.rec.n_fields; ++i) kai_decref(v->as.rec.fields[i]);
            free(v->as.rec.fields);
            free((void *) v->as.rec.names);
            break;
        case KAI_VARIANT: {
            /* Issue #440 — only pointer slots cascade through decref.
             * Phase 2: primitive slots (mask kind != PTR) carry raw
             * scalars and need no RC. The mask==0 hot path skips the
             * kind branch entirely. The mask lookup is hoisted out of the
             * slot loop — it was re-read per slot (5× for an RBNode), a
             * table lookup each time; the tag is fixed across the loop so
             * one read suffices. (The whole tree's free walks this.) */
            uint32_t fmask = kai_slot_mask_of(v->variant_tag);
            int fn_args = v->var_n_args;
            if (fmask == 0) {
                for (int i = 0; i < fn_args; ++i) kai_decref(kai_var_slots(v)[i].ptr);
            } else {
                for (int i = 0; i < fn_args; ++i) {
                    if (kai_var_slot_kind(fmask, i) == KAI_VAR_SLOT_PTR) {
                        kai_decref(kai_var_slots(v)[i].ptr);
                    }
                }
            }
        }
            /* FAM: payload slots are inline in this block — no separate
             * array to free. The whole block returns to the per-arity
             * variant pool at the recycle step below (keyed on tag). */
            break;
        case KAI_CLOSURE:
            for (int i = 0; i < v->as.clo.n_captures; ++i) kai_decref(v->as.clo.captures[i]);
            free(v->as.clo.captures);
            break;
        case KAI_ARRAY:
            for (int64_t i = 0; i < v->as.arr.len; ++i) kai_decref(v->as.arr.items[i]);
            free(v->as.arr.items);
            break;
        case KAI_REF:
            /* A Ref owns one strong reference to its cell. */
            kai_decref(v->as.ref.cell);
            break;
        case KAI_FIBER:
            if (v->as.fib) {
                kai_decref(v->as.fib->thunk);
                kai_decref(v->as.fib->result);
                /* Phase 5: free any remaining link nodes. Normally
                 * the trampoline's kai_link_propagate_terminate
                 * empties this chain; the safety net here covers
                 * fibers that died without going through the
                 * trampoline (or through this path before the
                 * trampoline ran). */
                {
                    KaiLinkNode *ln = v->as.fib->linked_head;
                    while (ln) {
                        KaiLinkNode *next = ln->next;
                        free(ln);
                        ln = next;
                    }
                    v->as.fib->linked_head = NULL;
                }
                /* Tier 2 Monitor — same safety net as the link chain.
                 * Each entry holds an owning ref on its target_pid via
                 * kai_monitor_add; we release that ref before freeing
                 * the node so values referenced only by the monitor
                 * chain can be reclaimed. */
                {
                    KaiMonitorNode *mn = v->as.fib->monitor_head;
                    while (mn) {
                        KaiMonitorNode *next = mn->next;
                        if (mn->target_pid) kai_decref(mn->target_pid);
                        free(mn);
                        mn = next;
                    }
                    v->as.fib->monitor_head = NULL;
                }
                /* R4 fix — when the trampoline drops the scheduler's
                 * ref on its own wrapper at DONE/CANCELLED, the wrapper
                 * may go to RC=0 here while we are still running on
                 * the fiber's private stack. Freeing the stack now
                 * would yank the ground out from under the trampoline
                 * tail. Defer the struct + stack free to the next
                 * fiber's drain hook (top of trampoline / post-swap
                 * in yield/park). The single-slot pending pointer is
                 * sufficient because the only producer is the
                 * trampoline tail, and every consumer drains before
                 * any other produce can run. */
                if (v->as.fib == kai_current_fiber()) {
                    v->as.fib->value = NULL;
                    kai_pending_free = v->as.fib;
                } else {
                    /* m8.x: release the private stack. Main fiber has
                     * stack_base == NULL (uses the OS thread stack)
                     * and is statically allocated; spawned fibers own
                     * an mmap region of stack_size + guard page bytes
                     * and must release it via munmap. */
                    if (v->as.fib->stack_base) {
                        munmap(v->as.fib->stack_base,
                               v->as.fib->stack_size + kai_page_size());
                    }
                    free(v->as.fib);
                }
            }
            break;
        case KAI_PID:
            /* The mailbox is owned by the with_mailbox / spawn_actor
             * scope, NOT by the Pid value. Dropping a Pid handle
             * does not free the mailbox. */
            break;
        case KAI_BYTE:
            /* Lane 4 (#473): Byte is a 1-byte scalar embedded directly
             * in the KaiValue. No heap payload. The base free(v)
             * below reclaims the whole value. */
            break;
        default: break;
    }
    /* Issue #817 — recycle this cell (trace+poison+pool+counters). The
     * same path kai_free_cons_spine uses per spine cell. */
    KAI_RECYCLE_CELL(v);
    KAI_PROF_EXIT(free);
}

/* Out-of-line drop-to-zero path: the cell reached rc==0, reclaim it.
 * Split out of kai_decref so the common case (decrement, still live)
 * stays a small inlinable body and only the rare free crosses a call.
 * Idea adapted from Koka's kk_block_decref / kk_block_drop split: the
 * drop fast path is inline at the call site, the free is a cold helper.
 * kaikai keeps its own counters + kai_free_value walker, not kklib's
 * free-list shape — the split is the idea, the body stays ours. */
static void kai_decref_free(KaiValue *v) {
#ifdef KAI_PROFILE_RC
    kai_prof_decref_to_zero_n++;
#endif
    kai_free_value(v);
}

/* Decrement a reference. The fast path — immediate Int / null /
 * saturated singleton / still-shared after decrement — is branch-only
 * and inlines into the caller; only a cell hitting rc==0 calls out to
 * kai_decref_free. Previously kai_decref was a non-inline `static void`
 * (callgrind: 48M instructions on the rb-tree bench, a real call at
 * every drop). Under tracing the counters still fire for full fidelity.
 * KAI_PROF_ENTER/EXIT dropped from the hot path: they bracket a cold
 * helper now, and the inline body must stay small to be inlined. */
static inline void kai_decref(KaiValue *v) {
    if (kai_is_value(v) || !v || v->rc == INT32_MAX) return;
    /* #812 — always-compiled counter (see kai_incref). */
    kai_rc_decref_total++;
#ifdef KAI_TRACE_RC
    kai_rc_history_log(v, /* op=decref */ 2, v->tag);
#endif
    if (--v->rc == 0) kai_decref_free(v);
}

/* m5 #4 — Perceus dup/drop wrappers callable as KaiValue-returning fns.
 *
 * `__perceus_dup` and `__perceus_drop` are AST-level magic names the
 * `perceus_pass` rewrites onto non-last EVar reads (dup) and unused-
 * binding scope ends (drop). The emitter lowers them to these
 * wrappers so the pass operates entirely through the prelude path
 * without bespoke C generation.
 *
 * `kai_internal_dup` is `kai_incref` with a typed return; the AST
 * rewrite wraps `EVar(x)` as `ECall(EVar("__perceus_dup"), [EVar(x)])`
 * and the emitter sees a normal call.
 *
 * `kai_internal_drop` decrefs and returns `unit`. Drops emit as
 * `SExprStmt(ECall(EVar("__perceus_drop"), [EVar(x)]))` so the unit
 * return goes into the discarded `KaiValue *_` slot of the existing
 * `SExprStmt` lowering. */
static KaiValue *kai_internal_dup(KaiValue *v) { return kai_incref(v); }
static KaiValue *kai_internal_drop(KaiValue *v) { kai_decref(v); return &kai_singleton_unit; }

/* ---------- constructors ---------- */

static KaiValue *kai_unit(void) { return &kai_singleton_unit; }

static KaiValue *kai_bool(int b) {
    return b ? &kai_singleton_true : &kai_singleton_false;
}

/* Char cache (Char is not yet a value-immediate). Every cached entry
 * carries `rc = INT32_MAX` so `kai_incref` / `kai_decref` skip them.
 *
 * The Int cache that used to live here is GONE: with the Koka
 * tagged-value Int (see the section near struct KaiValue) a small Int
 * is an immediate with no heap footprint, so there is nothing to
 * cache. The old [-65536..65535] x 48 B .bss table and its lazy-warm
 * branch were the m5.x patch that approximated value-immediates; its
 * own comment named "tagged pointers + raw-slot extract" as the real
 * follow-up, which is exactly this port. */
#define KAI_CHAR_CACHE_HI  ((uint32_t) 127)
#define KAI_CHAR_CACHE_SIZE 128

static KaiValue kai_char_cache[KAI_CHAR_CACHE_SIZE];
static int kai_char_cache_init = 0;

/* Per-entry lazy warm for the Int cache (Phase 1.A, 2026-05-29).
 *
 * The widened range (131072 entries x 48 B = 6 MB) makes a one-shot
 * O(size) warm a 6 MB RSS hit on the FIRST in-range int — paid even
 * by a program that only ever mints `0` and `1`. Measured: a hello
 * world jumped 1.5 MB -> 6.8 MB under the eager warm.
 *
 * Instead we initialize each cache slot the first time its exact value
 * is requested. A never-warmed slot is all-zero (.bss), i.e.
 * rc == 0 != INT32_MAX, so the `rc != INT32_MAX` test detects it. This
 * touches only the OS pages (4 KB) backing the int values actually
 * used, so RSS scales with the program's working set of small ints,
 * not with the cache range. The pinned `rc = INT32_MAX` still makes
 * `kai_incref` / `kai_decref` short-circuit, and the returned pointer
 * is stable for the process lifetime (slots are never re-warmed once
 * pinned). One extra branch per in-range `kai_int`, predictably taken
 * after the slot is hot. (CPython's small-int cache warms eagerly; we
 * warm lazily because our range — and entry size — is far larger.) */
static void kai_char_cache_warm(void) {
    for (int k = 0; k < KAI_CHAR_CACHE_SIZE; k++) {
        kai_char_cache[k].rc = INT32_MAX;
        kai_char_cache[k].tag = KAI_CHAR;
        kai_char_cache[k].as.c = (uint32_t) k;
    }
    kai_char_cache_init = 1;
}

/* Koka kk_integer_from_small (integer.h:211-215): a small Int is an
 * immediate value — no heap, no RC. Only out-of-range Ints (Koka's
 * bigint path) heap-allocate a KAI_INT fallback. The 5 MB int cache is
 * gone; immediacy makes it pointless.
 *
 * `kai_int` is the inline fast path: a 63-bit-fitting value (the
 * overwhelming common case — every loop counter, key, index) becomes a
 * tagged immediate with one shift and no call. Only the out-of-range
 * bignum tail detours to the NOINLINE heap-alloc helper. Before this
 * split `kai_int` was wholly NOINLINE, so reading an Int back from a
 * KAI_VAR_SLOT_INT slot (`kai_int(slot.i64)`) was a real call at every
 * field touch — the round-trip that made the raw-i64 slot kind look
 * slower than the tagged-ptr slot. With the fast path inlined the slot
 * read is a shift, the slot stays KAI_VAR_SLOT_INT (so the generic drop
 * walker skips it — no decref on keys), and both wins compose. */
static KAI_RC_NOINLINE KaiValue *kai_int_big(int64_t i) {
    KaiValue *v = kai_alloc(KAI_INT);
    v->as.i = i;
    return v;
}
static inline KaiValue *kai_int(int64_t i) {
    if (kai_int_fits_immediate(i)) return kai_tagged_int(i);
    return kai_int_big(i);
}

/* Uniform Int accessors — understand BOTH the immediate and the heap
 * fallback forms, so every hot op (kai_op_*) and every emitter unbox
 * site reads an Int the same way and the boxed↔unboxed frontier (the
 * re-box-on-every-boundary that the rb-tree bench measured) is gone.
 * Mirror of Koka's kk_integer accessors (integer.h). */
static inline int kai_is_int(KaiValue *v) {
    return kai_is_value(v) || (v != NULL && v->tag == KAI_INT);
}
static inline int64_t kai_intf(KaiValue *v) {
    /* The immediate (tagged) form is the overwhelming common case — every
     * loop counter, key, index. Only the 63-bit-overflow bignum tail
     * (|n| > 2^62) detours to the heap `v->as.i` load, which cannot be
     * speculated (immediate `v` is not a valid address). We keep the branch
     * (soundness: heap bignums are real) but hint it so the predictor and
     * layout favour the immediate path. The heap load stays off the hot
     * path. */
    intptr_t iv = (intptr_t) v;
    if (__builtin_expect((iv & KAI_INT_TAG_BIT) != 0, 1)) return iv >> 1;
    return v->as.i;
}

static KAI_RC_NOINLINE KaiValue *kai_real(double r) {
    KaiValue *v = kai_alloc(KAI_REAL);
    v->as.r = r;
    return v;
}

/* Lane 4 (#473): Byte nominal scalar. No interning cache for v1 — every
 * `kai_byte(n)` allocates a fresh KaiValue. Cache (analogous to
 * kai_int_cache for 0..127) is a natural Lane-4b/perf optimisation. */
static KAI_RC_NOINLINE KaiValue *kai_byte(uint8_t n) {
    KaiValue *v = kai_alloc(KAI_BYTE);
    v->as.byte_val = n;
    return v;
}

/* FFI v2 (#417): box an opaque C handle. `p` is borrowed external
 * memory — RC frees only this box, never `p` (see kai_free_value's
 * default arm: no payload free for KAI_FOREIGN). */
static KAI_RC_NOINLINE KaiValue *kai_foreign(void *p) {
    KaiValue *v = kai_alloc(KAI_FOREIGN);
    v->as.foreign_ptr = p;
    return v;
}

/* Unwrap an opaque handle back to its raw `void *` (a borrow — the box
 * keeps owning the cell; the pointer stays valid until the C resource
 * is destroyed by the driver). */
static inline void *kai_foreign_ptr(KaiValue *v) {
    return (v && v->tag == KAI_FOREIGN) ? v->as.foreign_ptr : NULL;
}

static KAI_RC_NOINLINE KaiValue *kai_char(uint32_t c) {
    if (c <= KAI_CHAR_CACHE_HI) {
        if (!kai_char_cache_init) kai_char_cache_warm();
        return &kai_char_cache[c];
    }
    KaiValue *v = kai_alloc(KAI_CHAR);
    v->as.c = c;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_str_from_bytes(const char *bytes, size_t len) {
    KaiValue *v = kai_alloc(KAI_STR);
    v->as.s.len = len;
    v->as.s.bytes = (char *) kai_heap_malloc(len + 1);
    if (len > 0) memcpy(v->as.s.bytes, bytes, len);
    v->as.s.bytes[len] = '\0';
    return v;
}

/* Lane FIX (top-3 leak sites): short-string interning.
 *
 * `prelude_table()` (DIAG rank #1) reallocates ~50 strings per call —
 * every call to `prelude_find` rebuilds the same `.rodata`-pointed
 * literals. The strings are content-deduped by the C compiler, so
 * repeat `kai_str("print")` calls receive the *same* `cstr` pointer.
 * Interning by pointer-then-content gives an immortal singleton per
 * literal, eliminating the per-call alloc/leak the same way
 * #300 / #304 did for nullary and immortal-payload variants.
 *
 * Two-stage lookup (open-addressed, 1024 buckets):
 *   1. Pointer match — hits 100% of compiler-emitted literal calls
 *      because identical literals share a single .rodata entry.
 *   2. Content match (≤ 64 bytes) — backstop for cstr's that arrive
 *      via stack buffers (`int_to_string`, `kai_cat2(...)`) and
 *      happen to repeat content.
 *
 * Misses (long strings, full table, transient content unique per
 * call) fall through to `kai_str_from_bytes` unchanged.
 *
 * Cached values carry `rc = INT32_MAX` so `kai_incref` / `kai_decref`
 * short-circuit; `kai_free_value` is never reached. */
#define KAI_STR_INTERN_BUCKETS 1024
#define KAI_STR_INTERN_MAXLEN  64
typedef struct {
    const char *cstr;       /* NULL ⇒ empty bucket. Owned via strdup. */
    size_t      len;
    KaiValue   *value;
} KaiStrInternBucket;
static KaiStrInternBucket kai_str_intern_table[KAI_STR_INTERN_BUCKETS];

static inline size_t kai_str_intern_hash(const char *cstr, size_t len) {
    /* FNV-1a, len-bounded. */
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t) (uint8_t) cstr[i];
        h *= 1099511628211ull;
    }
    return (size_t) (h & (KAI_STR_INTERN_BUCKETS - 1));
}

static KAI_RC_NOINLINE KaiValue *kai_str(const char *cstr) {
    size_t len = strlen(cstr);
    if (len > KAI_STR_INTERN_MAXLEN) return kai_str_from_bytes(cstr, len);
    size_t i = kai_str_intern_hash(cstr, len);
    for (size_t probe = 0; probe < KAI_STR_INTERN_BUCKETS; probe++) {
        KaiStrInternBucket *b = &kai_str_intern_table[i];
        if (b->cstr == NULL) {
            /* Insert. Allocate the immortal value + own a copy of cstr
             * (cstr arg may be a stack buffer that gets reused). */
            KaiValue *v = kai_str_from_bytes(cstr, len);
            v->rc = INT32_MAX;
            char *owned = (char *) malloc(len + 1);
            if (owned) {
                memcpy(owned, cstr, len);
                owned[len] = '\0';
                b->cstr = owned;
                b->len = len;
                b->value = v;
            }
            /* Even if strdup failed, the value is still valid (just
             * not cacheable). Subsequent calls re-alloc but stay
             * correct. */
            return v;
        }
        if (b->len == len && (b->cstr == cstr || memcmp(b->cstr, cstr, len) == 0)) {
            return b->value;
        }
        i = (i + 1) & (KAI_STR_INTERN_BUCKETS - 1);
    }
    /* Table full — fall back to non-cached. */
    return kai_str_from_bytes(cstr, len);
}

static KaiValue *kai_nil(void) { return &kai_singleton_nil; }

static KAI_RC_NOINLINE KaiValue *kai_cons(KaiValue *head, KaiValue *tail) {
    KaiValue *v = kai_alloc(KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

static KAI_RC_NOINLINE KaiValue *kai_record(int n, KaiValue **fields, const char **names) {
    KaiValue *v = kai_alloc(KAI_RECORD);
    v->as.rec.n_fields = n;
    v->as.rec.fields = (KaiValue **) malloc(n * sizeof(KaiValue *));
    v->as.rec.names  = (const char **) malloc(n * sizeof(const char *));
    v->as.rec.head_type_tag = 0;  /* anonymous; kai_record_h sets it nominally */
    for (int i = 0; i < n; ++i) {
        v->as.rec.fields[i] = fields[i];
        v->as.rec.names[i]  = names[i];
    }
    return v;
}

/* Nominal-record constructor — same as kai_record but stamps the
 * head-type tag for protocol dispatch. See docs/variant-tags.md
 * "Head-type tags". */
static KAI_RC_NOINLINE KaiValue *kai_record_h(int n, KaiValue **fields, const char **names, int32_t head_tag) {
    KaiValue *v = kai_record(n, fields, names);
    v->as.rec.head_type_tag = head_tag;
    return v;
}

/* Issue #300 — nullary variant singletons.
 *
 * `None` alone accounts for 50.7M allocations / 50.0M leaked in
 * the kaic2 self-compile (63.8% of total live count). Every
 * `kai_variant_u(_, "None", 0, 0, NULL)` call freshly allocs a chunk
 * structurally identical to every other `None` chunk: same tag,
 * same name pointer (string literal in .rodata), no payload. They
 * differ only in identity, which kaikai source code never observes
 * (variants don't have referential equality).
 *
 * The fix is the same trick already used for unit/bool/nil/cached
 * ints/chars: keep one immortal chunk per `(tag, name_ptr)` pair,
 * mark it `rc = INT32_MAX`, and return that pointer on every call.
 * `kai_incref` / `kai_decref` already short-circuit on INT32_MAX
 * so the singleton survives every RC operation unchanged and
 * `kai_free_value` is never reached with a singleton.
 *
 * Storage — open-addressed table keyed on (tag, name_ptr). The
 * name pointer is the address of a string literal in the binary's
 * .rodata segment, so it's stable across calls and unique per
 * distinct constructor. kaikai has on the order of 20-30 distinct
 * nullary variants; 64 buckets gives ample headroom.
 */
#define KAI_NULLARY_SINGLETON_BUCKETS 64
typedef struct {
    int32_t       tag;
    const char   *name;     /* NULL ⇒ empty bucket */
    KaiValue     *value;
} KaiNullarySingletonBucket;
static KaiNullarySingletonBucket
    kai_nullary_singletons[KAI_NULLARY_SINGLETON_BUCKETS];

static inline size_t kai_nullary_hash(int32_t tag, const char *name) {
    uintptr_t p = (uintptr_t) name;
    return (size_t) (((p >> 4) ^ (p >> 12) ^ (uintptr_t) tag)
                     & (KAI_NULLARY_SINGLETON_BUCKETS - 1));
}

static KaiValue *kai_nullary_lookup(int32_t tag, const char *name) {
    size_t i = kai_nullary_hash(tag, name);
    for (size_t probe = 0; probe < KAI_NULLARY_SINGLETON_BUCKETS; probe++) {
        KaiNullarySingletonBucket *b = &kai_nullary_singletons[i];
        if (b->name == NULL) return NULL;
        if (b->tag == tag && b->name == name) return b->value;
        i = (i + 1) & (KAI_NULLARY_SINGLETON_BUCKETS - 1);
    }
    return NULL;
}

/* Enum-slot fast path (KAI_VAR_SLOT_ENUM): a tag->singleton map so a
 * variant slot holding only the immediate `variant_tag` of an
 * all-nullary sum type can be re-boxed back into its interned
 * KaiValue* without knowing the ctor name at the read site. Tags are
 * dense small ints (>= 11 for user ctors); a flat array indexed by tag
 * is the cheapest lookup. Populated lazily by kai_nullary_install as
 * each nullary ctor is first constructed. Singletons are immortal
 * (rc == INT32_MAX), so the stored pointer never dangles. */
#define KAI_ENUM_TAG_MAX 1024
static KaiValue *kai_enum_by_tag[KAI_ENUM_TAG_MAX] = {0};

/* Fast nullary-ctor construction: read the interned singleton straight
 * from kai_enum_by_tag[tag] — an array load — instead of kai_variant_u's
 * NOINLINE call + nullary-cache hash+probe. Every nullary is seeded into
 * kai_enum_by_tag at startup (emit_nullary_enum_seed in
 * _kai_register_proto_tables, before main's body), so the load always
 * hits in steady state; the fallback covers a nullary built before the
 * seed (it self-installs, so later loads hit). On the rb-tree bench the
 * RBLeaf leaves were 13M instructions via the kai_variant_u path — every
 * insert-into-leaf mints two RBLeaf singletons through the hash probe
 * just to read back a pointer the seed already cached. Koka shape: a
 * nullary is kk_datatype_from_tag, an immediate, never a table lookup. */
static KAI_RC_NOINLINE KaiValue *kai_variant_u(int32_t tag, const char *name,
                                               int n, uint32_t mask,
                                               KaiVarSlot *slots);
static inline KaiValue *kai_nullary_fast(int32_t tag, const char *name) {
    if (tag >= 0 && tag < KAI_ENUM_TAG_MAX) {
        KaiValue *v = kai_enum_by_tag[tag];
        if (v) return v;
    }
    return kai_variant_u(tag, name, 0, 0, NULL);
}

static void kai_nullary_install(int32_t tag, const char *name, KaiValue *v) {
    if (tag >= 0 && tag < KAI_ENUM_TAG_MAX) kai_enum_by_tag[tag] = v;
    size_t i = kai_nullary_hash(tag, name);
    for (size_t probe = 0; probe < KAI_NULLARY_SINGLETON_BUCKETS; probe++) {
        KaiNullarySingletonBucket *b = &kai_nullary_singletons[i];
        if (b->name == NULL) {
            b->tag = tag;
            b->name = name;
            b->value = v;
            return;
        }
        i = (i + 1) & (KAI_NULLARY_SINGLETON_BUCKETS - 1);
    }
    /* Table full — fall back to non-singleton behaviour. With 64
     * buckets and a known cap of ~30 distinct nullary variants
     * this branch is unreachable in practice. */
}

/* Re-box an enum-slot's immediate tag into its interned singleton
 * KaiValue*. The singleton is created the first time the ctor is
 * constructed anywhere in the program (kai_variant_u nullary path,
 * which calls kai_nullary_install). For the slot to have been written
 * with this tag, that construction already happened, so the lookup
 * hits; the fallback (defensive) returns unit rather than dangle. */
static inline KaiValue *kai_enum_slot_box(int64_t tag) {
    if (tag >= 0 && tag < KAI_ENUM_TAG_MAX) {
        KaiValue *v = kai_enum_by_tag[(int) tag];
        if (v) return v;
    }
    return kai_unit();
}

/* Issue #293 next-tier — variants whose every arg is itself an
 * immortal singleton (rc==INT32_MAX) are themselves morally
 * immortal: they can never be mutated and have no observable
 * identity. Cache them just like nullary variants. The dominant
 * win is `Some(<immortal>)` and `Ok(<immortal>)`, which the
 * compiler builds by the millions when threading typer/parser
 * results that are themselves nullary variants or cached scalars.
 *
 * Storage — open-addressed table keyed on (tag, name, n, args[0..n]).
 * Capped at n <= 4 to bound the key size. Once the table fills,
 * subsequent lookups walk every bucket without finding the empty
 * sentinel, so installs silently fail and every later invocation
 * falls back to a fresh alloc. The 16384-bucket sizing originally
 * landed for #293 turned out to be ~16x too small for kaic2's
 * self-compile: the typer's `prelude_table` rebuilds 47 EP variants
 * per call × ~13K calls (issue #297 EP wave, 2026-05-07), and
 * Some/Ok/TyCon-shaped immortal-payload combinations push the
 * working set past 16K well before the prelude entries land. The
 * larger 262144-bucket sizing absorbs the EP table outright (642K
 * leaks → 45 cached chunks, 99.99% drop) and adds ~16 MB of static
 * .bss to the binary. Collisions still degrade to a fresh non-cached
 * alloc — same behaviour as a full nullary table.
 */
#define KAI_IMMORTAL_VAR_BUCKETS 262144
#define KAI_IMMORTAL_VAR_MAXN 4
typedef struct {
    int32_t       tag;
    int           n;
    const char   *name;     /* NULL ⇒ empty bucket */
    KaiValue     *args[KAI_IMMORTAL_VAR_MAXN];
    KaiValue     *value;
} KaiImmortalVarBucket;
static KaiImmortalVarBucket
    kai_immortal_vars[KAI_IMMORTAL_VAR_BUCKETS];

/* Reusable-tag registry (issue #118 layer 3) — the immortal-args
 * cache above immortalises any variant whose slots are all immortal,
 * on the #293 premise that such a cell "can never be mutated and has
 * no observable identity". That premise is FALSE for any tag the
 * program rewrites in place via a Perceus reuse-arm: `mirror`/`rotate`
 * over a `Tree` whose leaves are immortal `TLeaf` + interned strings
 * would immortalise every node (rc=INT32_MAX), and `kai_check_unique`
 * then rejects them — reuse-in-place never fires and the structure
 * leaks wholesale.
 *
 * The compiler's Perceus recogniser already knows exactly which tags
 * are reuse targets (it emits `kai_reuse_or_alloc_variant` for them).
 * It stamps that set here at startup via `kai_register_reusable_tags`.
 * `kai_variant_u` consults the set and skips immortalisation for a
 * reusable tag, leaving its cells with a real refcount so reuse can
 * fire. Some/Ok/AST-`Expr` (no reuse-arm) keep the #293 win intact;
 * only the tags the program actually mutates leave the cache.
 *
 * Storage — a bitset indexed by tag. Tags are dense small ints
 * assigned by the emitter; 1<<16 covers every realistic program with
 * a single 8 KiB static array. Out-of-range tags (defensive) are
 * treated as non-reusable, i.e. cacheable as before. */
#define KAI_REUSABLE_TAG_MAX 65536
static unsigned char kai_reusable_tag_bits[KAI_REUSABLE_TAG_MAX / 8];

static void kai_register_reusable_tags(const int32_t *tags, int n) {
    for (int i = 0; i < n; i++) {
        int32_t t = tags[i];
        if (t >= 0 && t < KAI_REUSABLE_TAG_MAX) {
            kai_reusable_tag_bits[t >> 3] |= (unsigned char) (1u << (t & 7));
        }
    }
}

static inline int kai_tag_is_reusable(int32_t tag) {
    if (tag < 0 || tag >= KAI_REUSABLE_TAG_MAX) return 0;
    return (kai_reusable_tag_bits[tag >> 3] >> (tag & 7)) & 1;
}

/* Issue #688 — single variant constructor.
 *
 * `kai_variant_u` is the only entry point for KAI_VARIANT construction.
 * Replaces the older `kai_variant` / `kai_variant_u` pair that diverged
 * during Phase 2 #440: the typed path took the slot-mask shape but
 * skipped the nullary / immortal caches, while the legacy boxed path
 * kept the caches but assumed mask==0. Now a single function carries
 * both: the cache fast paths fire when `mask == 0` (all slots are
 * boxed `.ptr` values), and the typed path falls through when any
 * primitive slot is present.
 *
 * Slot semantics: `slots[i].ptr` transfers ownership for pointer slots
 * exactly like the legacy `KaiValue **args` shape; `slots[i].i64` /
 * `slots[i].r` carry the raw unboxed payload for typed slots and have
 * no ref-count discipline. The mask bit `1 << i` selects pointer (bit
 * clear) vs primitive (bit set); convention matches stage 2's
 * `variant_slot_mask` emitter. */

static int kai_slots_all_immortal_ptr(int n, KaiVarSlot *slots) {
    if (n <= 0 || n > KAI_IMMORTAL_VAR_MAXN) return 0;
    for (int i = 0; i < n; i++) {
        /* An immediate value (tagged Int) has no header and is never
         * freed — it counts as immortal. A heap value is immortal only
         * when its rc is saturated. */
        /* A tagged-Int immediate slot DISQUALIFIES the variant from the
         * immortal cache. The cache is for variants of BOUNDED cardinality
         * (nullary ctors, all-immortal-pointer cells like `TLeaf` or
         * interned-string payloads): caching them wins because a small fixed
         * set is reused. A tagged Int has no header so it never frees — but a
         * variant CONTAINING a *variable* tagged Int (`Lit(i)` for arbitrary
         * `i`) has UNBOUNDED distinct identities. Immortalising it interns one
         * cache entry per distinct `i`, saturating the fixed open-addressing
         * table and degrading its linear probe to O(n) — the `variant_match`
         * super-linear collapse (issue #855). The C backend never hits this:
         * it builds such cells via `kai_variant_u_fast` with a TYPED `.i64`
         * slot, bypassing the mask==0 cache path entirely. Excluding tagged-Int
         * slots here brings the native (all-boxed) path to parity. */
        if (kai_is_value(slots[i].ptr)) return 0;
        if (slots[i].ptr == NULL || slots[i].ptr->rc != INT32_MAX) return 0;
    }
    return 1;
}

static inline size_t kai_immortal_slot_hash(int32_t tag, const char *name,
                                            int n, KaiVarSlot *slots) {
    uintptr_t h = (uintptr_t) name;
    h = (h * 1315423911u) ^ ((uintptr_t) tag);
    h = (h * 1315423911u) ^ (uintptr_t) n;
    for (int i = 0; i < n; i++) {
        h = (h * 1315423911u) ^ (uintptr_t) slots[i].ptr;
    }
    h ^= h >> 13;
    return (size_t) (h & (KAI_IMMORTAL_VAR_BUCKETS - 1));
}

static int kai_immortal_slot_match(KaiImmortalVarBucket *b, int32_t tag,
                                   const char *name, int n, KaiVarSlot *slots) {
    if (b->name != name || b->tag != tag || b->n != n) return 0;
    for (int i = 0; i < n; i++) {
        if (b->args[i] != slots[i].ptr) return 0;
    }
    return 1;
}

static KaiValue *kai_immortal_slot_lookup(int32_t tag, const char *name,
                                          int n, KaiVarSlot *slots) {
    size_t i = kai_immortal_slot_hash(tag, name, n, slots);
    for (size_t probe = 0; probe < KAI_IMMORTAL_VAR_BUCKETS; probe++) {
        KaiImmortalVarBucket *b = &kai_immortal_vars[i];
        if (b->name == NULL) return NULL;
        if (kai_immortal_slot_match(b, tag, name, n, slots)) return b->value;
        i = (i + 1) & (KAI_IMMORTAL_VAR_BUCKETS - 1);
    }
    return NULL;
}

static void kai_immortal_slot_install(int32_t tag, const char *name, int n,
                                      KaiVarSlot *slots, KaiValue *v) {
    size_t i = kai_immortal_slot_hash(tag, name, n, slots);
    for (size_t probe = 0; probe < KAI_IMMORTAL_VAR_BUCKETS; probe++) {
        KaiImmortalVarBucket *b = &kai_immortal_vars[i];
        if (b->name == NULL) {
            b->tag = tag;
            b->name = name;
            b->n = n;
            for (int j = 0; j < n; j++) b->args[j] = slots[j].ptr;
            b->value = v;
            return;
        }
        i = (i + 1) & (KAI_IMMORTAL_VAR_BUCKETS - 1);
    }
    /* Table full — fall back to non-cached behaviour. */
}

static KAI_RC_NOINLINE KaiValue *kai_variant_u(int32_t tag, const char *name,
                                               int n, uint32_t mask,
                                               KaiVarSlot *slots) {
    KAI_VAR_NAME_ALLOC(name);
    /* Record tag→name and tag→slot_mask: neither is stored per node now
     * (Koka-packed header). Rare readers recover them via the tables. */
    kai_varname_register(tag, name);
    kai_slotmask_register(tag, mask);

    /* Nullary fast path — shares the singleton cache with all other
     * `Name`-tag nullaries. Mask is irrelevant when n == 0. */
    if (n == 0 && name != NULL) {
        KaiValue *cached = kai_nullary_lookup(tag, name);
        if (cached) return cached;
        KAI_VAR_NAME_REAL_ALLOC(name);
        KaiValue *v = kai_alloc(KAI_VARIANT);
        v->variant_tag = tag;
        v->var_n_args = 0;
        kai_slotmask_register(v->variant_tag, 0);
        v->rc = INT32_MAX;
        kai_nullary_install(tag, name, v);
        return v;
    }

    /* Immortal-args fast path — only when every slot is a boxed pointer
     * (mask == 0) and every pointer is itself immortal. Cells with
     * primitive slots are not cached: their identity is the bit
     * pattern, which the existing cache shape does not key on.
     *
     * Issue #118 layer 3 — a tag the program rewrites in place (reuse
     * target) is excluded: immortalising it would saturate its rc and
     * permanently block `kai_check_unique`, killing reuse-in-place on
     * every cell of that type. The reusable-tag bitset is stamped at
     * startup by the compiler's Perceus recogniser. */
    if (mask == 0 && name != NULL && !kai_tag_is_reusable(tag) &&
        kai_slots_all_immortal_ptr(n, slots)) {
        KaiValue *cached = kai_immortal_slot_lookup(tag, name, n, slots);
        if (cached) return cached;
        KAI_VAR_NAME_REAL_ALLOC(name);
        KaiValue *v = kai_alloc_var(n);
        v->variant_tag = tag;
        v->var_n_args = n;
        kai_slotmask_register(v->variant_tag, 0);
        for (int i = 0; i < n; ++i) kai_var_slots(v)[i] = slots[i];
        v->rc = INT32_MAX;
        kai_immortal_slot_install(tag, name, n, slots, v);
        return v;
    }

    /* Cold path — fresh alloc, no caching. FAM: header + n inline slots
     * in one block; `slots` aliases the trailing inline_slots so the
     * emitted codegen reads payload identically. */
    KAI_VAR_NAME_REAL_ALLOC(name);
    KaiValue *v = kai_alloc_var(n);
    v->variant_tag = tag;
    v->var_n_args = n;
    kai_slotmask_register(v->variant_tag, mask);
    if (n > 0) {
        for (int i = 0; i < n; ++i) kai_var_slots(v)[i] = slots[i];
    } else {
    }
    return v;
}

/* Fast typed constructor for a payload-carrying variant with at least
 * one PRIMITIVE slot (the only case the emitter routes here).
 *
 * `kai_variant_u` runs a per-call preamble before the actual
 * alloc+stores: KAI_VAR_NAME_ALLOC, kai_varname_register,
 * kai_slotmask_register, then a nullary-singleton probe and an
 * immortal-args cache probe (262144-bucket hash + linear scan). It is
 * also KAI_RC_NOINLINE — a real call with argument spill at every one of
 * millions of construction sites (perfil: kai_variant_u = 43 samples,
 * second only to the loop body itself).
 *
 * For a cell with a primitive slot NONE of that preamble can help: the
 * nullary cache needs n == 0, and the immortal-args cache explicitly
 * skips cells with primitive slots ("their identity is the bit pattern,
 * which the cache shape does not key on"). So the only preamble work
 * that matters is registering tag→name and tag→mask — and both
 * registers are already idempotent (`kai_slotmask_seen[tag]` /
 * `kai_varname_table[tag]` guard the first write). This fast path keeps
 * exactly those two idempotent registers (so the generic drop walker
 * reads the correct mask) and drops everything else, inlined so the C
 * compiler folds the alloc + the fixed-bound store loop into the caller
 * — Koka's `kk_alloc(sizeof) + field stores` shape.
 *
 * Mask MUST be registered here (not assumed pre-stamped): the walker in
 * kai_free_value reads kai_slot_mask_of(tag) to decide which slots are
 * pointers. A missing mask would make it treat a raw i64 key as a
 * pointer and decref garbage. The `_seen` guard makes the steady-state
 * cost a single predictable branch. */
static inline KaiValue *kai_variant_u_fast(int32_t tag, int n,
                                           KaiVarSlot *slots) {
    /* No tag→name/mask register here: every payload ctor's name+mask is
     * stamped ONCE at startup by kai_register_payload_ctors (emitted in
     * _kai_register_proto_tables). Keeping the registers in the body made
     * the fn too large for the inliner — at -O3 it stayed a separate
     * 31.8M-instruction function (callgrind). Reduced to alloc+stores it
     * inlines into the call site, the Koka kk_alloc + field-stores shape.
     * No-zero alloc: the loop writes all n slots, so the allocator's
     * zero-init is dead work. */
    KaiValue *v = kai_alloc_var_nz(n);
    v->variant_tag = tag;
    v->var_n_args = (uint8_t) n;
    for (int i = 0; i < n; ++i) kai_var_slots(v)[i] = slots[i];
    return v;
}

/* Stamp the tag→name and tag→mask tables for every payload-carrying
 * constructor at startup, so kai_variant_u_fast needs no per-call
 * register (and stays small enough to inline). The emitter generates the
 * (tag, name, mask) triples table and one call to this in
 * _kai_register_proto_tables, beside kai_register_reusable_tags. */
typedef struct { int32_t tag; const char *name; uint32_t mask; } KaiPayloadCtor;
static void kai_register_payload_ctors(const KaiPayloadCtor *ctors, int n) {
    for (int i = 0; i < n; ++i) {
        kai_varname_register(ctors[i].tag, ctors[i].name);
        kai_slotmask_register(ctors[i].tag, ctors[i].mask);
    }
}

/* issue #120 — opt-in Perceus regions (Phase P1): arena-backed
 * constructors. Mirror kai_cons / kai_record / kai_variant_u but
 * bump-allocate the header (and interior arrays) into the current
 * region arena instead of the RC heap. The emitter routes every
 * constructor lexically inside a `region { }` block through these when
 * `cx.in_region` is set; the resulting nodes carry rc = INT32_MAX (the
 * arena sentinel) so RC skips them and the whole arena frees in one
 * shot at block exit. No active region (defensive — codegen bug) →
 * fall back to the RC constructor (correct, just unpooled). The arena
 * variant ctor does NOT intern nullaries (unlike kai_variant_u): an
 * arena nullary must die with its arena, not join the process-lifetime
 * singleton cache. */
static KaiValue *kai_arena_cons(KaiValue *head, KaiValue *tail) {
    KaiArena *ar = kai_arena_current();
    if (!ar) return kai_cons(head, tail);
    KaiValue *v = kai_arena_alloc(ar, KAI_CONS);
    v->as.cons.head = head;
    v->as.cons.tail = tail;
    return v;
}

static KaiValue *kai_arena_record(int n, KaiValue **fields, const char **names) {
    KaiArena *ar = kai_arena_current();
    if (!ar) return kai_record(n, fields, names);
    KaiValue *v = kai_arena_alloc(ar, KAI_RECORD);
    KaiValue **af = (KaiValue **) kai_arena_raw(ar, (size_t) (n > 0 ? n : 1) * sizeof(KaiValue *));
    const char **an = (const char **) kai_arena_raw(ar, (size_t) (n > 0 ? n : 1) * sizeof(const char *));
    for (int i = 0; i < n; ++i) { af[i] = fields[i]; an[i] = names[i]; }
    v->as.rec.n_fields = n;
    v->as.rec.fields = af;
    v->as.rec.names = an;
    v->as.rec.head_type_tag = 0;
    return v;
}

static KaiValue *kai_arena_variant(int32_t tag, const char *name, int n,
                                   uint32_t mask, KaiVarSlot *slots) {
    KaiArena *ar = kai_arena_current();
    if (!ar) return kai_variant_u(tag, name, n, mask, slots);
    KaiValue *v = kai_arena_alloc(ar, KAI_VARIANT);
    v->variant_tag = tag;
    v->var_n_args = n;
    kai_slotmask_register(v->variant_tag, mask);
    if (n > 0) {
        KaiVarSlot *as = (KaiVarSlot *) kai_arena_raw(ar, (size_t) n * sizeof(KaiVarSlot));
        for (int i = 0; i < n; ++i) as[i] = slots[i];
    } else {
    }
    return v;
}

/* issue #120 — deep-copy-out at the region border (Phase P1). When a
 * value built inside a `region { }` crosses out of the block (its
 * final-expression value), the emitter calls this to clone it OUT of
 * the arena onto the ordinary RC heap with rc = 1, BEFORE the arena is
 * bulk-freed. Gate on TAG (not on rc == INT32_MAX, which singletons
 * share with arena values): structural CONS/RECORD/VARIANT/ARRAY/STR
 * are reallocated fresh via kai_alloc (rc = 1) with fresh interior
 * arrays and recursively-copied children; scalar / singleton / opaque
 * leaves are kai_incref'd and shared (no interior pointers into the
 * arena, or already on the RC heap). */
static KaiValue *kai_deep_copy_out(KaiValue *v) {
    if (!v) return v;
    /* Koka tagged-Int: an immediate small Int has no heap header, so
     * `v->tag` would dereference a fake pointer. It is shared-nothing
     * by construction — copy-out returns it verbatim (incref is a
     * no-op for immediates, matching the default branch). */
    if (kai_is_value(v)) return v;
    switch ((KaiTag) v->tag) {
        case KAI_CONS: {
            KaiValue *h = kai_deep_copy_out(v->as.cons.head);
            KaiValue *t = kai_deep_copy_out(v->as.cons.tail);
            return kai_cons(h, t);
        }
        case KAI_RECORD: {
            int n = v->as.rec.n_fields;
            KaiValue **fields = (KaiValue **) malloc((size_t) (n > 0 ? n : 1) * sizeof(KaiValue *));
            if (!fields) { fprintf(stderr, "kai: out of memory (region copy-out)\n"); exit(1); }
            for (int i = 0; i < n; ++i) fields[i] = kai_deep_copy_out(v->as.rec.fields[i]);
            KaiValue *c = kai_record(n, fields, v->as.rec.names);
            c->as.rec.head_type_tag = v->as.rec.head_type_tag;
            free(fields);
            return c;
        }
        case KAI_VARIANT: {
            int n = v->var_n_args;
            if (n <= 0) return kai_variant_u(v->variant_tag, kai_variant_name_of(v->variant_tag), 0, 0, NULL);
            KaiVarSlot *slots = (KaiVarSlot *) malloc((size_t) n * sizeof(KaiVarSlot));
            if (!slots) { fprintf(stderr, "kai: out of memory (region copy-out)\n"); exit(1); }
            for (int i = 0; i < n; ++i)
                slots[i].ptr = kai_deep_copy_out(kai_var_slots(v)[i].ptr);
            KaiValue *c = kai_variant_u(v->variant_tag, kai_variant_name_of(v->variant_tag), n,
                                        kai_slot_mask_of(v->variant_tag), slots);
            free(slots);
            return c;
        }
        case KAI_ARRAY: {
            int64_t len = v->as.arr.len;
            KaiValue *c = kai_alloc(KAI_ARRAY);
            c->as.arr.len = len;
            c->as.arr.cap = len > 0 ? len : 1;
            c->as.arr.items = (KaiValue **) kai_heap_malloc((size_t) c->as.arr.cap * sizeof(KaiValue *));
            for (int64_t i = 0; i < len; ++i)
                c->as.arr.items[i] = kai_deep_copy_out(v->as.arr.items[i]);
            return c;
        }
        case KAI_STR: {
            size_t len = v->as.s.len;
            KaiValue *c = kai_alloc(KAI_STR);
            c->as.s.len = len;
            c->as.s.bytes = (char *) kai_heap_malloc(len + 1);
            if (len > 0) memcpy(c->as.s.bytes, v->as.s.bytes, len);
            c->as.s.bytes[len] = '\0';
            return c;
        }
        default:
            return kai_incref(v);
    }
}

/* Issue #440 Phase 2 — borrow a slot as a boxed `KaiValue *`. Used by
 * match-arm extraction, by the generic walkers (eq, to_string) and by
 * any other code path that needs the boxed view. For pointer slots
 * this returns the boxed child directly (still owned by the cell).
 * For primitive slots this allocates a fresh boxed temporary that the
 * caller owns and must release. Hot path: pointer slots are the
 * majority by far in legacy code; the branch predictor sees one
 * direction. */
static inline KaiValue *kai_variant_slot_box(KaiValue *v, int i) {
    uint32_t k = kai_var_slot_kind(kai_slot_mask_of(v->variant_tag), i);
    if (k == KAI_VAR_SLOT_PTR)  return kai_var_slots(v)[i].ptr;
    if (k == KAI_VAR_SLOT_INT)  return kai_int(kai_var_slots(v)[i].i64);
    if (k == KAI_VAR_SLOT_REAL) return kai_real(kai_var_slots(v)[i].r);
    if (k == KAI_VAR_SLOT_ENUM) return kai_enum_slot_box(kai_var_slots(v)[i].i64);
    return kai_var_slots(v)[i].ptr;
}

/* Issue #440 Phase 2 — consume a boxed Int / Real, return the raw
 * scalar and release the boxed temporary. Used by the stage 2
 * emitter when packing a primitive payload into a typed variant
 * slot: the call expression yields a boxed `KaiValue *` (already
 * possibly cached as a singleton), and we want the raw payload to
 * write into `slot.i64` / `slot.r`. Singleton Ints (rc == INT32_MAX)
 * survive the decref unchanged. */
static inline int64_t kai_take_int(KaiValue *v) {
    if (kai_is_value(v)) return kai_untag_int(v);   /* immediate: no header to decref */
    int64_t x = v->as.i;
    kai_decref(v);
    return x;
}
static inline double kai_take_real(KaiValue *v) {
    double x = v->as.r;
    kai_decref(v);
    return x;
}
/* Enum-slot pack: read the variant_tag of a (nullary, interned) sum
 * value and release the box. The box is an immortal singleton
 * (rc == INT32_MAX), so the decref is a no-op; we keep it for symmetry
 * with kai_take_int and to stay correct if a non-interned enum value
 * ever reaches here. Defensive on non-variant input (returns 0). */
static inline int64_t kai_take_enum(KaiValue *v) {
    int64_t tag = (v && kai_is_ptr(v) && v->tag == KAI_VARIANT)
                      ? (int64_t) v->variant_tag : 0;
    kai_decref(v);
    return tag;
}

/* ---------- Perceus reuse-in-place (issue #118 / Anga Roa wave) ----------
 *
 * Koka-style reuse-in-place: when a constructor consumes a value the
 * Perceus pass can prove uniquely owned (RC == 1), reuse the consumed
 * cell as the storage for the new value instead of paired free + alloc.
 *
 * Calling convention — these helpers are invoked from inside a
 * `match` arm body. The match emitter (`emit_match_default`) wraps
 * the scrutinee as `_scr` and *always* `kai_decref(_scr)`s on exit.
 * To avoid a double-free / use-after-free on a reused cell, these
 * helpers `kai_incref` the cell on the unique branch — the extra
 * ref balances the post-arm `kai_decref(_scr)`, leaving the caller
 * with a unique (RC=1) cell. The fallback branch leaves `_scr`
 * untouched; the match exit reclaims it normally.
 *
 * Discipline summary (per branch):
 *   - Unique: rewrite children in place, `kai_incref(_scr)`, return
 *     `_scr`. Net RC after match exit: 1.
 *   - Not unique / wrong shape: leave `_scr` alone, allocate fresh
 *     via the existing `kai_<shape>` constructor. Net RC after match
 *     exit: original_rc - 1.
 *
 * `head` / `tail` / `fields` / `args` are owning refs on entry — the
 * allocator branch hands them straight to `kai_<shape>` (which does
 * not incref); the reuse branch decref's the outgoing children
 * before storing the incoming ones, exactly as `kai_free_value`
 * would have during a paired free + alloc.
 *
 * The trace counter `kai_rc_reuse_total` increments on every
 * successful in-place rewrite (see top of file).
 */
static inline int kai_check_unique(KaiValue *v) {
    /* `rc == 1` already implies `rc != INT32_MAX` (a singleton's rc is
     * saturated at INT32_MAX, never 1), so the explicit singleton guard
     * the old code carried was a dead check executed on every call —
     * and check_unique runs at every descent level + twice per nested
     * balance arm. One `rc == 1` test suffices; an immediate (kai_is_
     * value) has no header so it is excluded first. Koka's
     * kk_datatype_ptr_is_unique is likewise a single refcount test.
     * Also marked inline (was a plain `static int` — a real call on the
     * hot path). */
    return v != NULL && !kai_is_value(v) && v->rc == 1;
}

/* Conditional incref for a variant reuse arm whose donor is SHARED. When the
 * donor reuses in place (unique), its kept children MOVE into the rebuild — no
 * incref. When it is shared, the rebuild fresh-allocs and keeps a borrowed
 * reference to each kept child, but the donor still owns those children and the
 * match-exit `kai_decref(donor)` will cascade-free them; so each kept child the
 * rebuild embeds needs its own ref. The emitter calls this once per embedded
 * borrowed child (multiset: a child used twice is incref'd twice). Idempotent
 * vs the reuse op's own uniqueness test — nothing mutates the donor between. */
static inline void kai_incref_if_shared(KaiValue *donor, KaiValue *child) {
    if (!kai_check_unique(donor)) kai_incref(child);
}

static KaiValue *kai_reuse_or_alloc_cons(KaiValue *_scr,
                                         KaiValue *head, KaiValue *tail) {
    if (_scr != NULL && _scr->tag == KAI_CONS && kai_check_unique(_scr)) {
        /* Aliasing guard (see kai_reuse_or_alloc_variant): the incoming
         * head/tail may alias the outgoing ones when `f` reused the
         * child cell in place. Store first, decref the old only if it
         * differs — decref-then-store would free a cell the new slot
         * still points at. Latent under libc malloc, exposed by the
         * cell free-list's immediate recycle. */
        KaiValue *old_h = _scr->as.cons.head;
        KaiValue *old_t = _scr->as.cons.tail;
        _scr->as.cons.head = head;
        _scr->as.cons.tail = tail;
        if (old_h != head) kai_decref(old_h);
        if (old_t != tail) kai_decref(old_t);
        kai_rc_reuse_total++;
        return kai_incref(_scr);   /* survive enclosing match-exit decref */
    }
    return kai_cons(head, tail);
}

static KaiValue *kai_reuse_or_alloc_record(KaiValue *_scr,
                                           int n, KaiValue **fields,
                                           const char **names) {
    if (_scr != NULL && _scr->tag == KAI_RECORD &&
        _scr->as.rec.n_fields == n && kai_check_unique(_scr)) {
        /* Decref the existing field values then overwrite. The
         * fields[] / names[] pointer arrays are reused — both have
         * the same length n, so no realloc is needed. names[]
         * usually points at the same static-string literals across
         * rebuilds (parsers thread the same record shape), so
         * overwriting is idempotent in the common case. */
        for (int i = 0; i < n; ++i) {
            /* Aliasing guard (see kai_reuse_or_alloc_variant). */
            KaiValue *old_f = _scr->as.rec.fields[i];
            _scr->as.rec.fields[i] = fields[i];
            _scr->as.rec.names[i]  = names[i];
            if (old_f != fields[i]) kai_decref(old_f);
        }
        kai_rc_reuse_total++;
        return kai_incref(_scr);
    }
    return kai_record(n, fields, names);
}

/* Same as kai_reuse_or_alloc_record but stamps head_type_tag — used by
 * codegen when the record's nominal head is known at compile time. */
static KaiValue *kai_reuse_or_alloc_record_h(KaiValue *_scr,
                                             int n, KaiValue **fields,
                                             const char **names,
                                             int32_t head_tag) {
    KaiValue *v = kai_reuse_or_alloc_record(_scr, n, fields, names);
    v->as.rec.head_type_tag = head_tag;
    return v;
}

static KaiValue *kai_reuse_or_alloc_variant(KaiValue *_scr,
                                            int32_t tag, const char *name,
                                            int n, uint32_t mask,
                                            KaiVarSlot *slots) {
    if (_scr != NULL && _scr->tag == KAI_VARIANT &&
        _scr->var_n_args == n && kai_slot_mask_of(_scr->variant_tag) == mask &&
        kai_check_unique(_scr)) {
        /* Phase 2: reuse fires when scrutinee and replacement agree on
         * the slot-mask layout. For mask==0 the scrutinee's slots are
         * all pointers and we must decref each one before overwriting.
         * For mask!=0 the matching slots are primitive payloads with
         * no RC discipline; the call sites that emit
         * `kai_reuse_or_alloc_variant` for typed payloads are
         * synthesised by stage 2's Perceus recogniser only when the
         * mask matches, so this branch stays sound.
         *
         * ALIASING GUARD (issue #118 #3 latent UAF, surfaced by the cell
         * free-list): the incoming `slots[i]` may ALIAS the outgoing
         * `_scr->slots[i]` — the common desugar shape
         * `Some(e) -> Some(f(e))` increfs `e`, computes `f(e)` (which
         * may reuse e's own cell in place, so the result pointer ==
         * the old slot pointer), then reuses `_scr`. Decref-then-store
         * would free the very cell the new slot points at. We decref the
         * OLD pointer only when it differs from the incoming one; an
         * aliased slot keeps the cell live (the incref/decref pair from
         * the bind + f's consumption already balanced it). With libc
         * malloc this latent double-free was masked (the freed cell
         * survived untouched until a far-off reuse); the immediate
         * free-list recycle exposed it. */
        for (int i = 0; i < n; ++i) {
            uint32_t bit = (uint32_t) 1 << i;
            KaiValue *old_ptr = kai_var_slots(_scr)[i].ptr;
            kai_var_slots(_scr)[i] = slots[i];
            if ((mask & bit) == 0 && old_ptr != slots[i].ptr) {
                kai_decref(old_ptr);
            }
        }
        _scr->variant_tag  = tag;
        kai_rc_reuse_total++;
        return kai_incref(_scr);
    }
    return kai_variant_u(tag, name, n, mask, slots);
}

/* ---------- TRMC constructor-context (port of Koka types-cctx.h) ----------
 *
 * Tail-Recursion-Modulo-Cons. Direct port of Koka's
 * `lib/std/core/inline/types-cctx.h` (Leijen & Lorenzen, "Tail Recursion
 * Modulo Context", POPL'22). A recursive spine-rebuild whose tail
 * expression is `Ctor(.., recur(child), ..)` is rewritten into a
 * `goto`-loop carrying a *constructor context* (`KaiCctx`): the partially
 * built result `res` plus a pointer `holeptr` to the open field where the
 * next node attaches. Each loop iteration builds one node with a HOLE in
 * the recursive slot, extends the cctx to point at that hole, and rebinds
 * the loop variable to the child — O(1) stack, perfect spine reuse when
 * the consumed cell is unique.
 *
 * kaikai difference from Koka: kaikai's holeptr points into the SEPARATE
 * `slots[]` array (`&kai_var_slots(node)[i].ptr`), not an inline field of
 * the block. The array stays alive and stable for the node's lifetime
 * (kai_variant_at overwrites in place; the fresh-alloc branch allocates a
 * fresh stable array), so the hole address never dangles. Koka's
 * unique-check is rc==0; kaikai's is rc==1 (see kai_check_unique).
 *
 * The `_linear` variants are what the compiler emits for affine effect
 * rows (the rb-tree's effect is total/affine). They assume `acc.res` is
 * unique — which TRMC guarantees because every node on the spine was just
 * built (or reused) by this loop and is held only by the cctx. Non-affine
 * (multi-resume) fns are NOT recognised by the TRMC pass; they fall back
 * to ordinary recursion, so the non-linear cctx path is not needed yet.
 */
typedef KaiValue **KaiFieldAddr;                  /* &kai_var_slots(node)[i].ptr */
typedef struct { KaiValue *res; KaiFieldAddr holeptr; } KaiCctx;

static inline KaiFieldAddr kai_field_addr_create(KaiValue **p) { return p; }

static inline KaiCctx kai_cctx_empty(void) { KaiCctx c = { NULL, NULL }; return c; }

/* apply_linear: plug `child` into the open hole, return the spine root.
 * Empty cctx (first level) → the child IS the root. Mirrors Koka
 * kk_cctx_apply_linear (types-cctx.h:66). */
static inline KaiValue *kai_cctx_apply_linear(KaiCctx acc, KaiValue *child) {
    if (acc.holeptr != NULL) { *acc.holeptr = child; return acc.res; }
    return child;
}

/* extend_linear: plug `child` into the old hole, return a new cctx whose
 * open hole is `field` (a slot inside `child`). Mirrors Koka
 * kk_cctx_extend_linear (types-cctx.h:91). */
static inline KaiCctx kai_cctx_extend_linear(KaiCctx acc, KaiValue *child,
                                             KaiFieldAddr field) {
    KaiCctx c;
    c.res = kai_cctx_apply_linear(acc, child);
    c.holeptr = field;
    return c;
}

/* Reuse token (Koka kk_datatype_ptr_reuse). The caller has already proven
 * `v` unique with kai_check_unique. Unlike kai_reuse_or_alloc_variant,
 * this does NOT decref the donor's children: the emitter MOVES them into
 * the new node (the unique branch) or dups+decrefs explicitly (the shared
 * branch), exactly as rbtree__koka.c:268-285. Returning the raw cell lets
 * kai_variant_at overwrite it in place. */
typedef KaiValue *KaiReuse;
#define kai_reuse_null NULL
static inline KaiReuse kai_ptr_reuse(KaiValue *v) { return v; }

/* arm-top drop-reuse, BORROW model (Koka kk_block_drop_reuse, kklib.h:818).
 * Runs at the TOP of a match arm whose binds are BORROW (children read from
 * `v`'s slots WITHOUT an incref), exactly like Koka's insert_loop:
 *
 *     tree l = con->left; ... r = con->right;      // borrow binds, no dup
 *     reuse_t _ru = kk_reuse_null;
 *     if (is_unique(t)) { _ru = reuse(t); }         // UNIQUE: take shell, children MOVE
 *     else { dup(l); dup(r); ...; decref(t); }      // SHARED: dup kept children, drop t
 *
 *   - UNIQUE (kaikai rc==1): return the bare cell as a token. Do NOT touch
 *     the children — they MOVE from this shell into the rebuilt node (the
 *     borrow binds become the owners; net RC 0). The emit does NOT dup them
 *     in the unique branch; kai_variant_at overwrites the slots next.
 *   - SHARED (rc>1): NON-recursive `rc--` only. The cell stays live for its
 *     other owners; its children stay referenced by it. The emit has dup'd
 *     the children it keeps in the shared branch. Return null → fresh alloc.
 *
 * `n` is the rebuild arity; n_args != n cannot host the rebuild. With borrow
 * binds the children are not owned here, so on mismatch we must NOT cascade:
 * refcount-only decrement, no token. (A mismatch is a recogniser bug.)
 *
 * CRITICAL vs kai_decref: the shared branch is a NON-recursive `rc--`. A
 * recursive decref would cascade into the borrow-bound children the emit is
 * about to use — the 14x-tree leak/UAF from the misplaced first attempt.
 * Koka's kk_block_drop_reuse is likewise refcount-only; child drops happen
 * via explicit emit-side dup/decref, never inside the reuse primitive. */
static inline KaiReuse kai_drop_reuse_token(KaiValue *v, int n) {
    if (v == NULL || kai_is_value(v) || v->tag != KAI_VARIANT ||
        v->var_n_args != n) {
        if (v != NULL && !kai_is_value(v) && v->tag == KAI_VARIANT &&
            v->rc != INT32_MAX) {
            v->rc -= 1;   /* non-recursive: children are borrow-bound, not ours */
        }
#ifdef KAI_TRACE_RC
        kai_rc_tok_null_mismatch++;
#endif
        return kai_reuse_null;
    }
    if (kai_check_unique(v)) {
        /* sole owner: hand back the shell. Children MOVE into the rebuild
         * (borrow binds become the owners). kai_variant_at overwrites next. */
#ifdef KAI_TRACE_RC
        kai_rc_tok_unique++;
#endif
        return v;
    }
    /* shared: non-recursive refcount decrement. */
    if (v->rc != INT32_MAX) v->rc -= 1;
#ifdef KAI_TRACE_RC
    kai_rc_tok_null_shared++;
#endif
    return kai_reuse_null;
}

/* alloc-at: write a variant Ctor into a donated cell IN PLACE (no malloc),
 * else fall back to a fresh alloc. Move semantics — does NOT decref the
 * donor's old slots (they were extracted into locals and either moved into
 * `slots[]` or dup'd by the emitter before donation). This is the correct
 * embryo; kai_reuse_or_alloc_variant's eager-1:1-decref is NOT (it
 * double-frees on non-bijective rebuilds — see lane memory). The donated
 * cell already has the right slots[] array length (n_args >= n checked);
 * we overwrite the slot words, retag, and reset rc=1. */
static inline KaiValue *kai_variant_at(KaiReuse at, int32_t tag,
                                       const char *name, int n, uint32_t mask,
                                       KaiVarSlot *slots) {
    if (at != NULL && at->tag == KAI_VARIANT && at->var_n_args == n) {
        for (int i = 0; i < n; ++i) kai_var_slots(at)[i] = slots[i];
        at->variant_tag = tag;
        /* The tag→mask register is NOT repeated here: a reuse target is,
         * by construction, a cell the program already built fresh (via
         * kai_variant_u / kai_variant_u_fast), so its mask is registered.
         * The slotmask table read by the drop walker is keyed on tag, not
         * on this cell, so the steady-state register was pure redundant
         * work (kai_slotmask_register's _seen guard already no-op'd it).
         * Dropping it removes a call from every in-place rebuild — the
         * rb-tree reuse arm fires ~20M times. The reuse counter only
         * exists under tracing; gate it so the steady path is store-only. */
        at->rc = 1;
#ifdef KAI_TRACE_RC
        kai_rc_reuse_total++;
#endif
        (void) mask; (void) name;
        return at;
    }
    return kai_variant_u(tag, name, n, mask, slots);
}

/* Free a reuse-TOKEN whose children have already MOVED out (Koka
 * kk_block_drop / ParcReuse's drop of an unconsumed `Available` cell).
 * A reuse-token is captured at the arm top (kai_drop_reuse_token UNIQUE
 * branch) WITHOUT touching its children — the borrow binds become the
 * owners. When the arm body reaches a tail that does NOT host a
 * kai_variant_at rebuild (e.g. `balance_left(insert_loop(l,...), ..., r)`
 * — the call allocates its own node inside its own frame), the token is
 * never consumed. ParcReuse treats the reuse-token as LINEAR: consumed by
 * a Con@reuse, else dropped on the non-consuming path. This is that drop.
 *
 * CRITICAL: it must free ONLY the cell, never cascade into the children —
 * those were stolen (no incref) into owned binders the body is still using
 * (the rb-tree balance arm passes them to balance_left). We zero n_args so
 * the KAI_VARIANT case of kai_free_value skips the slot decref loop, then
 * route through the normal free path (poison / cell-pool / counters stay
 * correct). A null token is a no-op (the shared branch returned null). */
static inline void kai_reuse_free(KaiReuse at) {
    if (at == NULL) return;
    if (kai_is_value(at) || at->tag != KAI_VARIANT) return;
    /* Children already MOVED out — must NOT cascade-decref them. With the
     * packed header, `slot_mask` is now a tag→mask TABLE, not a per-node
     * field, so the old trick of stamping this node's mask "all-INT" would
     * corrupt the shared mask of EVERY node of this tag (the double-free /
     * non-exhaustive crash). Instead recycle the block DIRECTLY at its real
     * arity, skipping kai_free_value's child-decref loop entirely — exactly
     * the "no cascade" intent, done per-node. The trace/RC counters that
     * kai_free_value would bump are handled here too. */
    KAI_VAR_NAME_FREE(kai_variant_name_of(at->variant_tag));
    kai_var_block_free(at, at->var_n_args);
    kai_rc_free_total++;
    kai_rc_live_now--;
#ifdef KAI_TRACE_RC
    kai_rc_reuse_free_total++;
    /* Per-tag free accounting (issue #296 table). `kai_var_block_free`
     * recycles the block directly, bypassing `kai_free_value`, so the
     * per-tag `frees` counter would otherwise stay flat while `allocs`
     * climbed — inflating the reported `live=allocs-frees` by exactly the
     * reuse_free count even though the cells are genuinely reclaimed (RSS
     * flat). Mirror kai_free_value's bump so `live` reflects real liveness
     * under arm-top reuse (this surfaced as a false LEAK in the #747 trace
     * once the LLVM backend began freeing unconsumed reuse tokens). */
    kai_rc_free_by_tag[(int) KAI_VARIANT]++;
#endif
}

/* TRMC reuse-in-place (Koka kk_block_drop_reuse + kk_block_alloc_at,
 * fused for the TRMC modulo-cons site). `_scr` is the variant cell the
 * enclosing `match` arm just consumed. When it is UNIQUE and has the
 * target arity, donate it as the storage for the rebuilt node:
 * overwrite the slot words (MOVE semantics — the arm already extracted
 * the donor's children into locals via kai_incref, so we must NOT
 * decref them), retag, and `incref` the cell so it survives the
 * `kai_decref(_scr)` the match emits at arm exit (net rc after exit:
 * 1, a unique freshly-built node — exactly the kai_reuse_or_alloc_cons
 * discipline). When `_scr` is shared or wrong-arity, allocate a fresh
 * node and leave `_scr` for the match exit to reclaim normally.
 *
 * This is what collapses the rb-tree's RBNode rebuild churn: each
 * insert reuses the cells it walked instead of paired free+alloc,
 * approaching Koka's in-place cost. Koka's unique gate is rc==0;
 * kaikai's is rc==1 (kai_check_unique). */
static inline KaiValue *kai_variant_reuse_at(KaiValue *_scr, int32_t tag,
                                             const char *name, int n,
                                             uint32_t mask, KaiVarSlot *slots) {
    if (_scr != NULL && !kai_is_value(_scr) && _scr->tag == KAI_VARIANT &&
        _scr->var_n_args == n && kai_check_unique(_scr)) {
        for (int i = 0; i < n; ++i) kai_var_slots(_scr)[i] = slots[i];
        _scr->variant_tag = tag;
        kai_slotmask_register(_scr->variant_tag, mask);
        kai_rc_reuse_total++;
        return kai_incref(_scr);   /* survive the match-exit kai_decref(_scr) */
    }
    return kai_variant_u(tag, name, n, mask, slots);
}

/* Allocate an array of `len` slots, each initialised to `init`
   (incref'd once per slot). Caller owns the returned array. */
static KAI_RC_NOINLINE KaiValue *kai_array_make(int64_t len, KaiValue *init) {
    if (len < 0) { fprintf(stderr, "kai: array_make: negative length\n"); exit(1); }
    KaiValue *v = kai_alloc(KAI_ARRAY);
    v->as.arr.len = len;
    v->as.arr.cap = len > 0 ? len : 1;
    v->as.arr.items = (KaiValue **) kai_heap_malloc((size_t) v->as.arr.cap * sizeof(KaiValue *));
    for (int64_t i = 0; i < len; ++i) v->as.arr.items[i] = kai_incref(init);
    return v;
}

/* Grow `a` to length `new_len`, initialising new slots with `init`.
   Mutates in place, returns the same value (incref'd for the caller
   so the result can be bound just like kai_array_make). If new_len
   <= current len this is a no-op beyond the incref. */
static KaiValue *kai_array_grow_impl(KaiValue *a, int64_t new_len, KaiValue *init) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_grow: not an array\n"); exit(1);
    }
    if (new_len > a->as.arr.cap) {
        int64_t nc = a->as.arr.cap * 2;
        if (nc < new_len) nc = new_len;
        a->as.arr.items = (KaiValue **) kai_heap_realloc(a->as.arr.items,
            (size_t) a->as.arr.cap * sizeof(KaiValue *), (size_t) nc * sizeof(KaiValue *));
        a->as.arr.cap = nc;
    }
    for (int64_t i = a->as.arr.len; i < new_len; ++i) a->as.arr.items[i] = kai_incref(init);
    if (new_len > a->as.arr.len) a->as.arr.len = new_len;
    return kai_incref(a);
}

/* O(1) read. Callers own the returned reference. */
static KaiValue *kai_array_get_impl(KaiValue *a, int64_t i) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_get: not an array\n"); exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        fprintf(stderr, "kai: array_get: index %lld out of range (len=%lld)\n",
                (long long) i, (long long) a->as.arr.len); exit(1);
    }
    return kai_incref(a->as.arr.items[i]);
}

/* O(1) write. Takes ownership of `v`, decref's the previous slot.
   Returns the same array (incref'd) so callers can thread it. */
static KaiValue *kai_array_set_impl(KaiValue *a, int64_t i, KaiValue *v) {
    if (!a || a->tag != KAI_ARRAY) {
        fprintf(stderr, "kai: array_set: not an array\n"); exit(1);
    }
    if (i < 0 || i >= a->as.arr.len) {
        fprintf(stderr, "kai: array_set: index %lld out of range (len=%lld)\n",
                (long long) i, (long long) a->as.arr.len); exit(1);
    }
    kai_decref(a->as.arr.items[i]);
    a->as.arr.items[i] = v;
    return kai_incref(a);
}

/* m5.x #2: incref each captured value so the closure owns its own
 * reference. Symmetric with `kai_free_value` (KAI_CLOSURE branch),
 * which decrefs every capture on chain-free. Without this incref the
 * closure stores raw aliases of caller-owned bindings; the alias goes
 * dangling as soon as the caller's scope decrefs them — latent under
 * the loose runtime, a use-after-free under linear consumption. */
static KAI_RC_NOINLINE KaiValue *kai_closure(KaiFn fn, int arity, int n_captures, KaiValue **captures) {
    KaiValue *v = kai_alloc(KAI_CLOSURE);
    v->as.clo.fn = fn;
    v->as.clo.arity = arity;
    v->as.clo.n_captures = n_captures;
    if (n_captures > 0) {
        v->as.clo.captures = (KaiValue **) malloc(n_captures * sizeof(KaiValue *));
        for (int i = 0; i < n_captures; ++i) v->as.clo.captures[i] = kai_incref(captures[i]);
    } else {
        v->as.clo.captures = NULL;
    }
    return v;
}

/* Invoke a closure dynamically.
 *
 * Issue #298 — kai_apply consumes its closure argument. Pre-fix the
 * helper borrowed `clo`; emitters at every callsite (stage0 emit.c,
 * stage1/stage2 compiler.kai, LLVM kaix_apply) handed it the binding
 * raw and assumed the surrounding scope owned the ref. For let-bound
 * closures invoked exactly once (e.g. `let f = make_adder(n); f(10)`),
 * Perceus picked the `LUAt + count == 1 -> raw transfer` branch and
 * never inserted a drop — so the closure leaked one ref per call. The
 * 27.97% closure leak in kaic2 self-compile was driven by this shape.
 *
 * Post-fix the contract is symmetric with the rest of m5.x: every
 * KaiValue * passed to kai_apply is OWNED by the call. The dispatched
 * fn body cannot read `self` after its own return (it cannot — that
 * pointer is freed before the result hits the caller), and runtime
 * helpers (kai_prelude_map / _filter / _flat_map / _reduce / _each,
 * kai_fiber_trampoline) now incref the closure ahead of every loop
 * iteration so each kai_apply gets its own ref to consume; their
 * post-loop decref releases the original ref the helper was handed. */
static KaiValue *kai_apply(KaiValue *clo, int argc, KaiValue **argv) {
    if (!clo || clo->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: attempted to call a non-callable value\n");
        exit(1);
    }
    KaiValue *r = clo->as.clo.fn(clo, argv, argc);
    kai_decref(clo);
    return r;
}

/* ---------- field access helpers ---------- */

static KaiValue *kai_op_field(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_incref(rec->as.rec.fields[i]);
        }
    }
    fprintf(stderr, "kai: no such field `%s`\n", name); exit(1);
}

/* Constant-index field read. Same RC contract as kai_op_field (increfs the
   read field) but skips the strcmp: the emitter computes the slot from the
   static record type when known, relying on the desugar-canonicalised
   declaration-order layout. */
static KaiValue *kai_op_field_at(KaiValue *rec, int i) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    return kai_incref(rec->as.rec.fields[i]);
}

/* m5.x §4b sibling: borrow the field without incref. Used in pat_test
   paths where the caller only reads the field's tag / value to decide
   the arm match — no downstream consumer that would need an owned ref.
   Pre-fix every pat_test of a record-shaped pattern leaked one ref per
   field tested (kai_op_field always increfed; the test result was
   discarded). emit_pat_binds keeps using the incref-ing kai_op_field so
   bindings still own their own refs. */
static KaiValue *kai_op_field_borrow(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        fprintf(stderr, "kai: field access on non-record\n"); exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return rec->as.rec.fields[i];
        }
    }
    fprintf(stderr, "kai: no such field `%s`\n", name); exit(1);
}

/* ---------- equality ---------- */

static int kai_op_eq(KaiValue *a, KaiValue *b) {
    if (a == b) return 1;   /* identical word — covers two equal immediates (Koka kk_box_eq) */
    /* Immediate Int: equal iff both are Ints with the same value. An
     * immediate vs a heap value of any other type is unequal. Done
     * before any header deref, since an immediate has no header. */
    if (kai_is_value(a) || kai_is_value(b)) {
        return kai_is_int(a) && kai_is_int(b) && kai_intf(a) == kai_intf(b);
    }
    if (!a || !b) return 0;
    if (a->tag != b->tag) return 0;
    switch ((KaiTag) a->tag) {
        case KAI_UNIT: return 1;
        case KAI_BOOL: return a->as.b == b->as.b;
        case KAI_INT:  return a->as.i == kai_intf(b);
        case KAI_REAL: return a->as.r == b->as.r;
        case KAI_CHAR: return a->as.c == b->as.c;
        case KAI_STR:  return a->as.s.len == b->as.s.len &&
                              memcmp(a->as.s.bytes, b->as.s.bytes, a->as.s.len) == 0;
        case KAI_NIL:  return 1;
        case KAI_CONS:
            return kai_op_eq(a->as.cons.head, b->as.cons.head) &&
                   kai_op_eq(a->as.cons.tail, b->as.cons.tail);
        case KAI_VARIANT: {
            /* Eq dispatch: if a's head type has a custom impl Eq, route to it
             * before structural fallback. Covers root AND nested cases (this
             * fires on every recursive descent into a field of this kind).
             * kai_op_eq is NON-consuming, so we incref a/b for the impl (which
             * consumes them) and do NOT decref here. */
            int32_t _eq_head = kai_head_tag(a);
            void *_eq_fn = kai_lookup_impl(KAI_PROTO_EQ, KAI_OP_EQ_EQ, _eq_head);
            if (_eq_fn) {
                kai_incref(a); kai_incref(b);
                KaiValue *_eq_r = ((KaiValue *(*)(KaiValue *, KaiValue *)) _eq_fn)(a, b);
                int _eq_res = kai_op_truthy(_eq_r);
                kai_decref(_eq_r);
                return _eq_res;
            }
            if (a->variant_tag != b->variant_tag) return 0;
            if (a->var_n_args != b->var_n_args) return 0;
            /* Phase 2: if either cell carries primitive slots, the
             * masks must agree slot-by-slot (a variant of the same
             * tag/name must have the same layout). Then compare each
             * slot by its kind: pointer slots recurse via kai_op_eq,
             * primitive slots compare raw scalars. The mask==0 hot
             * path stays on the original pointer-by-pointer walk. */
            if (kai_slot_mask_of(a->variant_tag) == 0 && kai_slot_mask_of(b->variant_tag) == 0) {
                for (int i = 0; i < a->var_n_args; ++i) {
                    if (!kai_op_eq(kai_var_slots(a)[i].ptr, kai_var_slots(b)[i].ptr)) return 0;
                }
            } else {
                if (kai_slot_mask_of(a->variant_tag) != kai_slot_mask_of(b->variant_tag)) return 0;
                for (int i = 0; i < a->var_n_args; ++i) {
                    uint32_t k = kai_var_slot_kind(kai_slot_mask_of(a->variant_tag), i);
                    if (k == KAI_VAR_SLOT_PTR) {
                        if (!kai_op_eq(kai_var_slots(a)[i].ptr, kai_var_slots(b)[i].ptr)) return 0;
                    } else if (k == KAI_VAR_SLOT_INT || k == KAI_VAR_SLOT_ENUM) {
                        /* ENUM compares its immediate variant_tag like a raw
                         * Int; two enum slots are equal iff same tag. */
                        if (kai_var_slots(a)[i].i64 != kai_var_slots(b)[i].i64) return 0;
                    } else if (k == KAI_VAR_SLOT_REAL) {
                        if (kai_var_slots(a)[i].r != kai_var_slots(b)[i].r) return 0;
                    }
                }
            }
            return 1;
        }
        case KAI_RECORD: {
            /* Eq dispatch for records carrying a custom impl Eq. Records only
             * get a dispatchable head_type_tag when stamped at construction
             * (see LIMITATION in the handoff: nested record fields do not get
             * a tag today). Root records reached via the resolver still work;
             * this hook fires only when as.rec.head_type_tag is a real impl. */
            int32_t _eq_head = kai_head_tag(a);
            void *_eq_fn = kai_lookup_impl(KAI_PROTO_EQ, KAI_OP_EQ_EQ, _eq_head);
            if (_eq_fn) {
                kai_incref(a); kai_incref(b);
                KaiValue *_eq_r = ((KaiValue *(*)(KaiValue *, KaiValue *)) _eq_fn)(a, b);
                int _eq_res = kai_op_truthy(_eq_r);
                kai_decref(_eq_r);
                return _eq_res;
            }
            if (a->as.rec.n_fields != b->as.rec.n_fields) return 0;
            for (int i = 0; i < a->as.rec.n_fields; ++i) {
                if (!kai_op_eq(a->as.rec.fields[i], b->as.rec.fields[i])) return 0;
            }
            return 1;
        }
        case KAI_CLOSURE: return 0;      /* closures are not equatable */
        case KAI_ARRAY:   return 0;      /* arrays are opaque, identity-compared */
        case KAI_REF:     return 0;      /* refs are identity-compared; a==b handled above */
        case KAI_FIBER:   return a->as.fib == b->as.fib;  /* identity */
        case KAI_PID:     return a->as.mb  == b->as.mb;   /* identity */
        case KAI_FOREIGN: return a->as.foreign_ptr == b->as.foreign_ptr; /* identity (#417) */
        case KAI_BYTE:      return a->as.byte_val == b->as.byte_val;    /* Lane 4 (#473) */
    }
    return 0;
}

/* ---------- to-string ---------- */

static KAI_RC_NOINLINE KaiValue *kai_string_concat(KaiValue *a, KaiValue *b);

static KaiValue *kai_to_string(KaiValue *v);

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

static KaiValue *kai_list_to_string(KaiValue *v) {
    KaiValue *acc = kai_str("[");
    int first = 1;
    while (kai_is_ptr(v) && v->tag == KAI_CONS) {
        if (!first) {
            KaiValue *c = kai_string_concat(acc, kai_str(", "));
            kai_decref(acc); acc = c;
        }
        first = 0;
        KaiValue *s = kai_to_string(v->as.cons.head);
        KaiValue *c = kai_string_concat(acc, s);
        kai_decref(acc); kai_decref(s); acc = c;
        v = v->as.cons.tail;
    }
    KaiValue *c = kai_string_concat(acc, kai_str("]"));
    kai_decref(acc); return c;
}

static KaiValue *kai_to_string(KaiValue *v) {
    if (!v) return kai_str("<null>");
    char buf[64];
    /* Koka tagged-Int: a small Int is an immediate, not a heap value —
     * `v->tag` would dereference the fake pointer and crash. Render it
     * as the decoded integer before touching the header. */
    if (kai_is_value(v)) {
        snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
        return kai_str(buf);
    }
    switch ((KaiTag) v->tag) {
        case KAI_UNIT: return kai_str("()");
        case KAI_BOOL: return kai_str(v->as.b ? "true" : "false");
        case KAI_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
            return kai_str(buf);
        case KAI_REAL:
            snprintf(buf, sizeof(buf), "%g", v->as.r);
            return kai_str(buf);
        case KAI_CHAR:
            snprintf(buf, sizeof(buf), "%c", (char) v->as.c);
            return kai_str(buf);
        case KAI_STR:    return kai_incref(v);
        case KAI_NIL:    return kai_str("[]");
        case KAI_CONS:   return kai_list_to_string(v);
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
        case KAI_BYTE:                                       /* Lane 4 (#473) */
            snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
            return kai_str(buf);
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

/* ---------- prelude: IO ---------- */

/*
 * Calling convention for the prelude: borrow semantics. Arguments are
 * borrowed from the caller (no incref / no decref on them). Return
 * values are owned by the caller.
 */

static KaiValue *kai_prelude_print(KaiValue *arg) {
    if (!arg) { fputc('\n', stdout); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stdout);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
        kai_decref(s);
    }
    fputc('\n', stdout);
    kai_decref(arg);
    return kai_unit();
}

static KaiValue *kai_prelude_eprint(KaiValue *arg) {
    if (!arg) { fputc('\n', stderr); return kai_unit(); }
    if (arg->tag == KAI_STR) {
        fwrite(arg->as.s.bytes, 1, arg->as.s.len, stderr);
    } else {
        KaiValue *s = kai_to_string(arg);
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
        kai_decref(s);
    }
    fputc('\n', stderr);
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
static KaiValue *kai_prelude_write_stdout(KaiValue *arg) {
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

static KaiValue *kai_prelude_panic(KaiValue *msg) {
    fprintf(stderr, "panic: ");
    if (kai_is_ptr(msg) && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    exit(1);
    return kai_unit();
}

static KaiValue *kai_prelude_exit(KaiValue *code) {
    int c = (kai_is_int(code)) ? (int) kai_intf(code) : 0;
    exit(c);
    return kai_unit();
}

/* ---------- prelude: conversions ---------- */

static KaiValue *kai_prelude_int_to_string(KaiValue *v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long) kai_intf(v));
    KaiValue *r = kai_str(buf);
    kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_real_to_string(KaiValue *v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", v->as.r);
    KaiValue *r = kai_str(buf);
    kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_int_to_real(KaiValue *v) {
    int64_t n = (kai_is_int(v)) ? kai_intf(v) : 0;
    KaiValue *r = kai_real((double) n);
    kai_decref(v);
    return r;
}

/* Truncating cast toward zero. Out-of-range, NaN and Inf collapse to
 * 0 — kaikai has no IEEE 754 surface for the user yet, so the safer
 * default beats a UB conversion. Revisit when the math/real lane
 * adds NaN-aware predicates. */
static KaiValue *kai_prelude_real_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_REAL) { if (v) kai_decref(v); return kai_int(0); }
    double r = v->as.r;
    KaiValue *out;
    if (r != r) out = kai_int(0);                       /* NaN */
    else if (r >  9.2233720368547748e18) out = kai_int(0); /* > INT64_MAX */
    else if (r < -9.2233720368547758e18) out = kai_int(0); /* < INT64_MIN */
    else out = kai_int((int64_t) r);
    kai_decref(v);
    return out;
}

/* ---------- Lane 4 (#473): Byte nominal scalar conversions + ops ----- */

/* `int_to_byte(n)` returns `Result[String, Byte]` — Err if n is outside
 * 0..255, Ok otherwise. The Result is encoded as a KAI_VARIANT (Ok /
 * Err) with one payload. */
static KaiValue *kai_prelude_int_to_byte(KaiValue *v) {
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
        KaiValue *err = kai_str(buf);
        return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = err}});
    }
    KaiValue *ok_payload = kai_byte((uint8_t) n);
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = ok_payload}});
}

static KaiValue *kai_prelude_byte_to_int(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_int(0); }
    int64_t n = (int64_t) v->as.byte_val;
    kai_decref(v);
    return kai_int(n);
}

/* Wrapping arithmetic per uint8_t C semantics. Overflow is defined. */
static KaiValue *kai_prelude_byte_add(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av + bv));
}

static KaiValue *kai_prelude_byte_sub(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_byte((uint8_t) (av - bv));
}

static KaiValue *kai_prelude_byte_eq(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av == bv);
}

static KaiValue *kai_prelude_byte_lt(KaiValue *a, KaiValue *b) {
    uint8_t av = (kai_is_ptr(a) && a->tag == KAI_BYTE) ? a->as.byte_val : 0;
    uint8_t bv = (kai_is_ptr(b) && b->tag == KAI_BYTE) ? b->as.byte_val : 0;
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return kai_bool(av < bv);
}

static KaiValue *kai_prelude_byte_to_string(KaiValue *v) {
    if (!v || v->tag != KAI_BYTE) { if (v) kai_decref(v); return kai_str("0"); }
    char buf[16];
    snprintf(buf, sizeof(buf), "%u", (unsigned) v->as.byte_val);
    kai_decref(v);
    return kai_str(buf);
}

/* ---------- prelude: math/real libm bindings (issue #343) ----------
 * IEEE-754 pass-through: NaN / Inf propagate per C99 <math.h>.
 * Inspect with kai_prelude_real_is_nan / kai_prelude_real_is_inf. */

#define KAI_LIBM_REAL1(name, fn)                                         \
    static KaiValue *kai_prelude_real_##name(KaiValue *x) {              \
        double r = (kai_is_ptr(x) && x->tag == KAI_REAL) ? fn(x->as.r) : 0.0;        \
        KaiValue *out = kai_real(r);                                     \
        if (x) kai_decref(x);                                            \
        return out;                                                      \
    }

#define KAI_LIBM_REAL2(name, fn)                                         \
    static KaiValue *kai_prelude_real_##name(KaiValue *a, KaiValue *b) { \
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
static KaiValue *kai_prelude_real_signum(KaiValue *x) {
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

static KaiValue *kai_prelude_real_is_nan(KaiValue *x) {
    int yes = 0;
    if (kai_is_ptr(x) && x->tag == KAI_REAL) { double r = x->as.r; yes = (r != r); }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
    return out;
}

static KaiValue *kai_prelude_real_is_inf(KaiValue *x) {
    int yes = 0;
    if (kai_is_ptr(x) && x->tag == KAI_REAL) {
        double r = x->as.r;
        yes = (r > 1.7976931348623157e308) || (r < -1.7976931348623157e308);
    }
    KaiValue *out = kai_bool(yes);
    if (x) kai_decref(x);
    return out;
}

/* ---------- prelude: strings ---------- */

static KaiValue *kai_prelude_string_length(KaiValue *s) {
    int64_t n = (kai_is_ptr(s) && s->tag == KAI_STR) ? (int64_t) s->as.s.len : 0;
    KaiValue *r = kai_int(n);
    if (s) kai_decref(s);
    return r;
}

static KaiValue *kai_prelude_string_concat(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_string_concat(a, b);
    if (a) kai_decref(a);
    if (b) kai_decref(b);
    return r;
}

static KaiValue *kai_prelude_string_concat_all(KaiValue *xs) {
    KaiValue *r = kai_string_concat_all_impl(xs);
    if (xs) kai_decref(xs);
    return r;
}

static KaiValue *kai_prelude_string_join(KaiValue *xs, KaiValue *sep) {
    KaiValue *r = kai_string_join_impl(xs, sep);
    if (xs) kai_decref(xs);
    if (sep) kai_decref(sep);
    return r;
}

/* ---------- prelude: arrays ---------- */

static KaiValue *kai_prelude_array_make(KaiValue *n, KaiValue *init) {
    int64_t len = (kai_is_int(n)) ? kai_intf(n) : 0;
    /* impl increfs `init` once per slot; consume our own input
     * refs (n and init) at the boundary. */
    KaiValue *r = kai_array_make(len, init);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

static KaiValue *kai_prelude_array_length(KaiValue *a) {
    int64_t len = (kai_is_ptr(a) && a->tag == KAI_ARRAY) ? a->as.arr.len : 0;
    KaiValue *r = kai_int(len);
    if (a) kai_decref(a);
    return r;
}

static KaiValue *kai_prelude_array_get(KaiValue *a, KaiValue *i) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    KaiValue *r = kai_array_get_impl(a, idx);
    if (a) kai_decref(a);
    if (i) kai_decref(i);
    return r;
}

static KaiValue *kai_prelude_array_set(KaiValue *a, KaiValue *i, KaiValue *v) {
    int64_t idx = (kai_is_int(i)) ? kai_intf(i) : 0;
    /* impl returns kai_incref(a) — a fresh ref the caller owns.
     * Our input `a` ref is therefore redundant under the callee-
     * consumes convention; decref it so the array's refcount only
     * reflects (1) the slot self-ref + (2) the caller's returned
     * ref, never the borrowed entry. */
    KaiValue *r = kai_array_set_impl(a, idx, kai_incref(v));
    if (a) kai_decref(a);
    if (i) kai_decref(i);
    if (v) kai_decref(v);
    return r;
}

static KaiValue *kai_prelude_array_grow(KaiValue *a, KaiValue *n, KaiValue *init) {
    int64_t new_len = (kai_is_int(n)) ? kai_intf(n) : 0;
    /* impl returns kai_incref(a) — caller owns the new ref. The
     * input `a` ref is consumed under the callee-consumes
     * convention. */
    KaiValue *r = kai_array_grow_impl(a, new_len, init);
    if (a) kai_decref(a);
    if (n) kai_decref(n);
    if (init) kai_decref(init);
    return r;
}

/* ---------- prelude: refs (issue #257) ----------
 *
 * `Ref[T]` is a single-cell mutable reference in the `Mutable`
 * effect (docs/effects-stdlib.md §Mutable). Surface ops:
 *   ref_make[T](init: T)        : Ref[T]
 *   ref_get[T](r: Ref[T])       : T
 *   ref_set[T](r: Ref[T], v: T) : Unit
 *
 * Runtime layout: a Ref is a KAI_REF — a single mutable cell with its
 * own tag (one `KaiValue *cell` field). Distinct from KAI_ARRAY: no
 * length, no capacity, no indexing — the "a Ref is exactly one slot"
 * invariant lives in the representation, not just the surface type.
 * (The earlier length-1 KAI_ARRAY hack from #257 is replaced here.)
 * RC: a Ref owns one strong reference to its cell. All three ops obey
 * the callee-consumes convention. */
static KaiValue *kai_prelude_ref_make(KaiValue *init) {
    /* Steal `init` straight into the cell — no incref/decref dance
     * (the array-backed version did one of each; this is strictly less
     * RC work, which matters on the hot path of Front A). */
    KaiValue *r = kai_alloc(KAI_REF);
    r->as.ref.cell = init;
    return r;
}

static KaiValue *kai_prelude_ref_get(KaiValue *r) {
    /* Hand back a fresh strong reference to the cell; consume `r`. */
    KaiValue *v = r ? kai_incref(r->as.ref.cell) : NULL;
    if (r) kai_decref(r);
    return v;
}

static KaiValue *kai_prelude_ref_set(KaiValue *r, KaiValue *v) {
    /* Drop the old contents, steal `v` into the cell; consume `r`.
     * Surface contract is Unit-typed. No bounds check, no index. */
    if (r) {
        kai_decref(r->as.ref.cell);
        r->as.ref.cell = v;       /* steals v */
        kai_decref(r);
    } else if (v) {
        kai_decref(v);
    }
    return kai_unit();
}

/* ---------- prelude: lists ---------- */

static KaiValue *kai_prelude_list_length(KaiValue *xs) {
    int64_t n = 0;
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) { n++; p = p->as.cons.tail; }
    KaiValue *r = kai_int(n);
    if (xs) kai_decref(xs);
    return r;
}

/* Recursive helper: borrows `xs` (does NOT decref it), so the
 * recursion can walk the cons chain without consuming on the way
 * down. The public `kai_prelude_list_append` wraps this and
 * decrefs `xs`/`ys` after the chain is built. */
static KaiValue *kai_list_append_borrow(KaiValue *xs, KaiValue *ys) {
    if (!xs || xs->tag == KAI_NIL) return kai_incref(ys);
    KaiValue *rest = kai_list_append_borrow(xs->as.cons.tail, ys);
    return kai_cons(kai_incref(xs->as.cons.head), rest);
}

static KaiValue *kai_prelude_list_append(KaiValue *xs, KaiValue *ys) {
    KaiValue *r = kai_list_append_borrow(xs, ys);
    if (xs) kai_decref(xs);
    if (ys) kai_decref(ys);
    return r;
}

static KaiValue *kai_prelude_list_reverse(KaiValue *xs) {
    KaiValue *acc = kai_nil();
    KaiValue *p   = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        acc = kai_cons(kai_incref(p->as.cons.head), acc);
        p   = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    return acc;
}

/* ---------- prelude: higher order ---------- */
/*
 * Iterative refactor (Perceus Tier 2 part 3, 2026-04-29). Pre-flip
 * these were recursive over `xs->as.cons.tail`, which forced the
 * helpers to *borrow* `xs` (the recursion holds aliased pointers
 * to every cell while the closure runs). Now they walk iteratively
 * with `KaiValue *p = xs; p = p->as.cons.tail;` so the entire
 * cons-chain stays alive under `xs`'s single reference until the
 * helper exits, and we can decref `xs` once at the end.
 *
 * kai_apply contract under m5.x flip: every arg slot holds an OWNED
 * reference that the callee consumes (the closure body is perceus-
 * compiled and decrefs each arg through normal use). The helpers
 * `kai_incref(p->as.cons.head)` to give the closure its own
 * ownership of each element while the cons cell stays alive under
 * `xs`. `kai_apply` itself does NOT consume `f` — the helper does
 * that once, post-loop.
 *
 * `_map` / `_filter` build their result reversed (each iteration
 * cons-prepends) and call `kai_prelude_list_reverse` once to
 * restore order. `list_reverse` consumes its arg, so the
 * intermediate reversed list is freed in the same step that
 * produces the final list — no extra retention.
 */

static KaiValue *kai_prelude_map(KaiValue *xs, KaiValue *f) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        /* kai_apply consumes (#298): give it its own ref each iter. */
        KaiValue *head = kai_apply(kai_incref(f), 1, &arg0);
        acc = kai_cons(head, acc);
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

/* issue #201: runtime backing for the flat-map-pipe operator (`||`).
 * Mirrors `kai_prelude_map` but each `f(elem)` produces a list that
 * gets cons-prepended onto the reversed accumulator; the per-element
 * piece is decref'd in the same step. The final reverse + decref loop
 * matches `_map`'s ownership story so RC stays balanced regardless of
 * the input shape. Empty pieces are a no-op for the inner loop.
 */
static KaiValue *kai_prelude_flat_map(KaiValue *xs, KaiValue *f) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        /* kai_apply consumes (#298): incref f for each iter. */
        KaiValue *piece = kai_apply(kai_incref(f), 1, &arg0);
        /* Each `piece` is owned; append it onto `acc` (which is in
         * reverse order). `kai_prelude_list_append` consumes both
         * arguments. We append `acc` onto the front of `piece`'s
         * reversed form by reversing piece into acc cons-by-cons.
         */
        KaiValue *q = piece;
        while (kai_is_ptr(q) && q->tag == KAI_CONS) {
            acc = kai_cons(kai_incref(q->as.cons.head), acc);
            q = q->as.cons.tail;
        }
        if (piece) kai_decref(piece);
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return result;
}

static KaiValue *kai_prelude_filter(KaiValue *xs, KaiValue *pred) {
    KaiValue *acc = kai_nil();
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        /* kai_apply consumes (#298): incref pred for each iter. */
        KaiValue *keep = kai_apply(kai_incref(pred), 1, &arg0);
        int yes = kai_op_truthy(keep);
        kai_decref(keep);
        if (yes) {
            acc = kai_cons(kai_incref(p->as.cons.head), acc);
        }
        p = p->as.cons.tail;
    }
    KaiValue *result = kai_prelude_list_reverse(acc);  /* consumes acc */
    if (xs)   kai_decref(xs);
    if (pred) kai_decref(pred);
    return result;
}

static KaiValue *kai_prelude_reduce(KaiValue *xs, KaiValue *init, KaiValue *f) {
    /* acc starts as the caller's `init` ref (transferred). Each
     * iteration hands acc to the closure (which consumes it) and
     * receives a freshly-owned `next` back. If `xs` is empty we
     * return init unchanged — its single ref flows out to the
     * caller. */
    KaiValue *acc = init;
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        KaiValue *args[2];
        args[0] = acc;                              /* transfer to closure */
        args[1] = kai_incref(p->as.cons.head);      /* closure consumes */
        /* kai_apply consumes (#298): incref f for each iter. */
        acc = kai_apply(kai_incref(f), 2, args);    /* closure produces fresh acc */
        p = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return acc;
}

static KaiValue *kai_prelude_each(KaiValue *xs, KaiValue *f) {
    KaiValue *p = xs;
    while (kai_is_ptr(p) && p->tag == KAI_CONS) {
        KaiValue *arg0 = kai_incref(p->as.cons.head);
        /* kai_apply consumes (#298): incref f for each iter. */
        KaiValue *r = kai_apply(kai_incref(f), 1, &arg0);
        kai_decref(r);
        p = p->as.cons.tail;
    }
    if (xs) kai_decref(xs);
    if (f)  kai_decref(f);
    return kai_unit();
}

/* ---------- binary and unary operators ---------- */
/*
 * Linear-consumption primitives (m5.x-flip Phase 3, 2026-04-28):
 * each op reads ALL relevant fields of its arguments BEFORE decref'ing,
 * then returns a freshly-allocated result. Aliasing-safe: `kai_op_eq_v(x, x)`
 * decrefs `a` and `b` separately, but the read-then-decref ordering means
 * both reads complete on a still-alive value. This pairs with the dup
 * pass + exit drops + producer-side incref-on-extract (Steps A + B) so
 * every value is released exactly once. NOT flipped: `kai_op_field` (already
 * increfs its return; introspection rather than consumer), `kai_op_eq` (C-int
 * returning, used inside non-consuming match tests), `kai_apply` (closure
 * invocation; lifecycle managed at the call lowering).
 */

/* Int add, sub, and mul compute in uint64_t and cast back. kaikai's Int is a
 * wrapping two's-complement int64_t by design (CLAUDE.md Tier 1 + the
 * Int contract in docs); the polynomial/FNV hash mixing in
 * stdlib/protocols.kai relies on the wrap. Signed overflow is UB in
 * C99 (6.5) even when the hardware wraps, so doing the math directly
 * on int64_t trips `-fsanitize=undefined` (tier1-asan) the moment a
 * value overflows — which `impl Hash for Real` makes trivial (a
 * Real's bit-cast is a large Int, and `acc * 31` overflows). The
 * unsigned compute is well-defined modular arithmetic (6.3.1.3) and,
 * on every target kaikai supports (two's-complement clang/gcc),
 * produces the byte-identical result the signed form did — so this
 * silences the UB without changing any emitted output. Division and
 * comparison stay signed (their overflow is not part of the contract).
 */
static KaiValue *kai_op_add(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) + (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r + b->as.r);
    else { fprintf(stderr, "kai: type mismatch in +\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_sub(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) - (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r - b->as.r);
    else { fprintf(stderr, "kai: type mismatch in -\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mul(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_int((int64_t)((uint64_t) kai_intf(a) * (uint64_t) kai_intf(b)));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_real(a->as.r * b->as.r);
    else { fprintf(stderr, "kai: type mismatch in *\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_div(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a) && kai_is_int(b)) {
        if (kai_intf(b) == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
        r = kai_int(kai_intf(a) / kai_intf(b));
    } else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) {
        r = kai_real(a->as.r / b->as.r);
    } else { fprintf(stderr, "kai: type mismatch in /\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_idiv(KaiValue *a, KaiValue *b) {
    int64_t av = 0, bv = 0;
    if      (kai_is_int(a))  av = kai_intf(a);
    else if (kai_is_ptr(a) && a->tag == KAI_REAL) av = (int64_t) a->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if      (kai_is_int(b))  bv = kai_intf(b);
    else if (kai_is_ptr(b) && b->tag == KAI_REAL) bv = (int64_t) b->as.r;
    else { fprintf(stderr, "kai: type mismatch in //\n"); exit(1); }
    if (bv == 0) { fprintf(stderr, "kai: divide by zero\n"); exit(1); }
    KaiValue *r = kai_int(av / bv);
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_mod(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a) && kai_is_int(b)) {
        if (kai_intf(b) == 0) { fprintf(stderr, "kai: mod by zero\n"); exit(1); }
        r = kai_int(kai_intf(a) % kai_intf(b));
    } else { fprintf(stderr, "kai: type mismatch in %%\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_lt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_bool(kai_intf(a) < kai_intf(b));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_bool(a->as.r < b->as.r);
    else if (kai_is_ptr(a) && a->tag == KAI_CHAR && kai_is_ptr(b) && b->tag == KAI_CHAR) r = kai_bool(a->as.c < b->as.c);
    else if (kai_is_ptr(a) && a->tag == KAI_STR  && kai_is_ptr(b) && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c < 0);
        else        r = kai_bool(a->as.s.len < b->as.s.len);
    } else {
        /* Ord dispatch: route to custom impl Ord.cmp when present (root and
         * nested). kai_op_lt CONSUMES a/b, so incref for the impl (which also
         * consumes) AND decref a/b at the end. cmp result < 0 means a < b. */
        int32_t _o_head = kai_head_tag(a);
        void *_o_fn = kai_lookup_impl(KAI_PROTO_ORD, KAI_OP_ORD_CMP, _o_head);
        if (_o_fn) {
            kai_incref(a); kai_incref(b);
            KaiValue *_o_c = ((KaiValue *(*)(KaiValue *, KaiValue *)) _o_fn)(a, b);
            int _o_r = kai_intf(_o_c) < 0;
            kai_decref(_o_c);
            kai_decref(a); kai_decref(b);
            return kai_bool(_o_r);
        }
        fprintf(stderr, "kai: type mismatch in <\n"); exit(1);
    }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_gt(KaiValue *a, KaiValue *b) {
    KaiValue *r;
    if (kai_is_int(a)  && kai_is_int(b))       r = kai_bool(kai_intf(a) > kai_intf(b));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL && kai_is_ptr(b) && b->tag == KAI_REAL) r = kai_bool(a->as.r > b->as.r);
    else if (kai_is_ptr(a) && a->tag == KAI_CHAR && kai_is_ptr(b) && b->tag == KAI_CHAR) r = kai_bool(a->as.c > b->as.c);
    else if (kai_is_ptr(a) && a->tag == KAI_STR  && kai_is_ptr(b) && b->tag == KAI_STR) {
        size_t n = a->as.s.len < b->as.s.len ? a->as.s.len : b->as.s.len;
        int c = memcmp(a->as.s.bytes, b->as.s.bytes, n);
        if (c != 0) r = kai_bool(c > 0);
        else        r = kai_bool(a->as.s.len > b->as.s.len);
    } else {
        /* Ord dispatch (mirror of kai_op_lt). cmp result > 0 means a > b. */
        int32_t _o_head = kai_head_tag(a);
        void *_o_fn = kai_lookup_impl(KAI_PROTO_ORD, KAI_OP_ORD_CMP, _o_head);
        if (_o_fn) {
            kai_incref(a); kai_incref(b);
            KaiValue *_o_c = ((KaiValue *(*)(KaiValue *, KaiValue *)) _o_fn)(a, b);
            int _o_r = kai_intf(_o_c) > 0;
            kai_decref(_o_c);
            kai_decref(a); kai_decref(b);
            return kai_bool(_o_r);
        }
        fprintf(stderr, "kai: type mismatch in >\n"); exit(1);
    }
    kai_decref(a); kai_decref(b);
    return r;
}

/* `kai_op_le` and `kai_op_ge` are now layered over the consuming `kai_op_gt` /
 * `kai_op_lt`. The inner call consumes `a` and `b` already; we must not
 * decref them again here. The intermediate bool result is consumed
 * locally to read the inverted truth value. */
static KaiValue *kai_op_le(KaiValue *a, KaiValue *b) {
    KaiValue *g = kai_op_gt(a, b);
    KaiValue *r = kai_bool(!g->as.b);
    kai_decref(g);
    return r;
}

static KaiValue *kai_op_ge(KaiValue *a, KaiValue *b) {
    KaiValue *l = kai_op_lt(a, b);
    KaiValue *r = kai_bool(!l->as.b);
    kai_decref(l);
    return r;
}

/* `kai_op_eq` does NOT consume — it is the C-int returning equality used by
 * pattern tests and other non-consuming sites. `kai_op_eq_v` / `kai_op_ne_v`
 * are the value-level wrappers used for `==` / `!=` expressions; those
 * DO consume per the m5.x flip. Self-aliasing (`kai_op_eq_v(x, x)`) is safe
 * because `kai_op_eq` reads both pointers before either decref runs.  */
static KaiValue *kai_op_eq_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_ne_v(KaiValue *a, KaiValue *b) {
    KaiValue *r = kai_bool(!kai_op_eq(a, b));
    kai_decref(a); kai_decref(b);
    return r;
}

/* `kai_op_pow_int(a, b)` — integer-exponent power for the `^` operator.
 * Lives in runtime so the operator works without requiring the
 * Numeric protocol in scope. The unit of a dimensioned operand is
 * metadata on the type only; the runtime value carries no unit, so
 * the same code path serves `Real` and `Real<u>` alike.
 *
 * For Int: negative exponents truncate to 0 (no integer reciprocal).
 * For Real: negative exponents compute `1.0 / base^|e|` so unit
 * lifting at the type level (`r : Real<m>`, `r ^ -1 : Real<m^-1>`)
 * matches a meaningful runtime result. This is intentionally more
 * lenient than the `Numeric.pow_int` stdlib impl, which clamps
 * negatives to 0; the discrepancy is acceptable because `^`
 * dispatches through this helper, never through the protocol. */
static KaiValue *kai_op_pow_int(KaiValue *a, KaiValue *b) {
    if (!kai_is_int(b)) {
        fprintf(stderr, "kai: type mismatch in ^ (exponent must be Int)\n"); exit(1);
    }
    int64_t e = kai_intf(b);
    KaiValue *r;
    if (kai_is_int(a)) {
        if (e < 0) { r = kai_int(0); }
        else {
            int64_t base = kai_intf(a);
            int64_t acc = 1;
            for (int64_t i = 0; i < e; i++) { acc *= base; }
            r = kai_int(acc);
        }
    } else if (kai_is_ptr(a) && a->tag == KAI_REAL) {
        double base = a->as.r;
        int64_t k = e < 0 ? -e : e;
        double acc = 1.0;
        for (int64_t i = 0; i < k; i++) { acc *= base; }
        if (e < 0) { acc = 1.0 / acc; }
        r = kai_real(acc);
    } else { fprintf(stderr, "kai: type mismatch in ^\n"); exit(1); }
    kai_decref(a); kai_decref(b);
    return r;
}

static KaiValue *kai_op_neg(KaiValue *a) {
    KaiValue *r;
    if (kai_is_int(a))       r = kai_int(-kai_intf(a));
    else if (kai_is_ptr(a) && a->tag == KAI_REAL) r = kai_real(-a->as.r);
    else { fprintf(stderr, "kai: type mismatch in unary -\n"); exit(1); }
    kai_decref(a);
    return r;
}

static KaiValue *kai_op_boolnot(KaiValue *a) {
    KaiValue *r;
    if (a->tag == KAI_BOOL) r = kai_bool(!a->as.b);
    else { fprintf(stderr, "kai: type mismatch in `not`\n"); exit(1); }
    kai_decref(a);
    return r;
}

/* `kai_op_truthy` is a non-consuming C-int predicate used inside
 * `if (kai_op_truthy(...))`, ternary lowerings of `if`/`and`/`or`, and
 * `kai_assert_check`. It is intentionally NOT flipped under the
 * m5.x runtime flip: the LLVM short-circuit lowering's phi node
 * returns `lhs` itself in the early-exit branch, so consuming `lhs`
 * inside the truthiness probe would alias-free a value still
 * referenced downstream. The emitted-C path leaks the temporary
 * argument; tracked in issue #82 as future cleanup (predicate
 * consumes + emit-side incref before short-circuit). */
static int kai_op_truthy(KaiValue *v) {
    return v && v->tag == KAI_BOOL && v->as.b;
}

/* ---------- range construction ---------- */

static KaiValue *kai_range(KaiValue *from, KaiValue *to) {
    int64_t f = kai_intf(from), t = kai_intf(to);
    KaiValue *acc = kai_nil();
    for (int64_t i = t; i >= f; --i) acc = kai_cons(kai_int(i), acc);
    return acc;
}

static KaiValue *kai_range_step(KaiValue *from, KaiValue *to, KaiValue *step) {
    int64_t f = kai_intf(from), t = kai_intf(to), s = kai_intf(step);
    if (s == 0) { fprintf(stderr, "kai: zero step in range\n"); exit(1); }
    int64_t *buf = NULL;
    size_t cap = 0, n = 0;
    if (s > 0) {
        for (int64_t i = f; i <= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    } else {
        for (int64_t i = f; i >= t; i += s) {
            if (n == cap) { cap = cap ? cap * 2 : 16; buf = (int64_t *) realloc(buf, cap * sizeof(int64_t)); }
            buf[n++] = i;
        }
    }
    KaiValue *acc = kai_nil();
    for (size_t i = n; i > 0;) { --i; acc = kai_cons(kai_int(buf[i]), acc); }
    free(buf);
    return acc;
}

/* ---------- prelude: args (set by generated `int main`) ---------- */

static int          kai_g_argc = 0;
static char       **kai_g_argv = NULL;

static void kai_set_args(int argc, char **argv) {
    kai_g_argc = argc;
    kai_g_argv = argv;
    /* Issue #678: libc defaults stdout to fully-buffered when the fd
     * is a pipe or regular file. Combined with the runtime's reliance
     * on atexit-driven flush (which signal-driven termination skips),
     * that loses every Stdout.print issued before the buffer fills or
     * before a clean exit. Match Go / Rust / Python -u semantics by
     * forcing line-buffering at process entry. One syscall at startup;
     * TTY stdout is already line-buffered so this is a no-op there.
     * stderr is unbuffered by default — no change needed. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Lazy registration: kai_set_args runs once at program start from
       the emitted main wrapper. atexit fires only when the env var
       gates a report, so the side effect is harmless when tracing is
       off. Keeps the emitter unchanged — no new emit site needed. */
    kai_rc_register_once();
#ifdef KAI_TRACE_RC
    kai_rc_strict_register_once();
#endif
}

static KaiValue *kai_prelude_args(void) {
    KaiValue *acc = kai_nil();
    for (int i = kai_g_argc - 1; i >= 1; --i) {
        acc = kai_cons(kai_str(kai_g_argv[i]), acc);
    }
    return acc;
}

/* Issue #127: argv[0] as a kaikai String. Mirrors `kai_prelude_args`
 * in routing through the `kai_g_argv` snapshot installed by
 * `kai_set_args` at process entry. Returns the empty string when
 * argv was never captured (unit tests linking the runtime without
 * a generated `int main` wrapper) — leaves the surface total. */
static KaiValue *kai_prelude_program_name(void) {
    if (kai_g_argv == NULL || kai_g_argv[0] == NULL) return kai_str("");
    return kai_str(kai_g_argv[0]);
}

/* Hanga Roa core loader: the path where `core/` lives on this
 * system, hard-coded at compile time via -DKAI_STDLIB_PATH=... in
 * stage1/stage2 Makefiles. Defaults to "stdlib" (relative to the
 * caller's cwd) when the macro is unset, which keeps in-tree
 * unit harnesses happy without the macro. Surface to kaikai code
 * via the `kai_stdlib_path` builtin. */
#ifndef KAI_STDLIB_PATH
#define KAI_STDLIB_PATH "stdlib"
#endif
/* The compile-time `KAI_STDLIB_PATH` macro is set by stage{1,2}/Makefile
 * to `$(abspath ../stdlib)` — an absolute path to the checkout's stdlib
 * directory. After install (brew, tarball, etc.) that path no longer
 * exists, so the env-var override `KAIKAI_STDLIB_PATH` takes precedence
 * when set. `bin/kai` exports this var pointing at the installed
 * `share/kaikai/stdlib` so users get a working compiler without
 * rebuilding from source. Direct `kaic2` invocations from a source
 * checkout leave the env unset and fall back to the macro.
 */
static KaiValue *kai_prelude_stdlib_path(void) {
    const char *env = getenv("KAIKAI_STDLIB_PATH");
    if (env && *env) {
        return kai_str(env);
    }
    return kai_str(KAI_STDLIB_PATH);
}

/* Canonicalise a filesystem path via realpath(3). Returns the input
 * unchanged when the path does not yet exist (caller's "first_try"
 * candidate path before stat), so the dedup logic at the load site
 * stays correct: only paths that resolve to real files get folded
 * to their canonical form. Backed by POSIX realpath; falls back to
 * the input string when realpath fails (errno != 0). */
static KaiValue *kai_prelude_abspath(KaiValue *path) {
    if (!path || path->tag != KAI_STR) {
        return path;
    }
    char pbuf[KAI_PATH_BUF];
    size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
    memcpy(pbuf, path->as.s.bytes, plen);
    pbuf[plen] = '\0';
    char resolved[KAI_PATH_BUF];
    if (realpath(pbuf, resolved) == NULL) {
        return path;
    }
    return kai_str(resolved);
}

/* ---------- prelude: mailbox runtime (m8 #7) ---------- */

/* User code reaches the mailbox runtime through these prelude
 * functions. They are wrapped in stdlib/actor.kai's `with_mailbox`
 * helper, which also installs the user-facing Actor[Msg] handler.
 * The polymorphic surface uses Nothing → TyAny so a single set of
 * runtime entries serves every Msg type. */

static KaiValue *kai_prelude_mailbox_alloc(void) {
    return kai_pid_value(kai_mailbox_alloc());
}

/* Tier 2 spawn_actor — allocate a mailbox without stamping any
 * owner. Pair with kai_prelude_mailbox_assign_owner to wire the
 * mailbox onto the spawned fiber before its body runs. */
static KaiValue *kai_prelude_mailbox_alloc_unowned(void) {
    return kai_pid_value(kai_mailbox_alloc_unowned());
}

/* Tier 2 spawn_actor — set `pid->as.mb->owner_fiber = fiber->as.fib`
 * AND `fiber->as.fib->mailbox = pid->as.mb`. Used after
 * kai_prelude_mailbox_alloc_unowned + fiber_spawn so the spawned
 * fiber owns the mailbox for monitor / link / trap-exit lookups.
 *
 * Safe under the cooperative scheduler because fiber_spawn enqueues
 * the spawned fiber but does not yield — the parent runs through to
 * this assign call before the spawned trampoline gets the CPU. */
static KaiValue *kai_prelude_mailbox_assign_owner(KaiValue *pid, KaiValue *fiber) {
    if (pid && pid->tag == KAI_PID && pid->as.mb &&
        fiber && fiber->tag == KAI_FIBER && fiber->as.fib) {
        pid->as.mb->owner_fiber = fiber->as.fib;
        fiber->as.fib->mailbox  = pid->as.mb;
    }
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (pid)   kai_decref(pid);
    if (fiber) kai_decref(fiber);
    return kai_unit();
}

static KaiValue *kai_prelude_mailbox_alloc_bounded(KaiValue *cap, KaiValue *overflow) {
    int c = (kai_is_int(cap)) ? (int) kai_intf(cap) : 0;
    int o = (kai_is_int(overflow)) ? (int) kai_intf(overflow) : 0;
    KaiValue *r = kai_pid_value(kai_mailbox_alloc_bounded(c, o));
    /* m5.x flip Phase 3 closeout (issue #82): consume input refs. */
    if (cap)      kai_decref(cap);
    if (overflow) kai_decref(overflow);
    return r;
}

/* Issue #763 spawn_actor_policy — bounded alloc without stamping an
 * owner. Pair with kai_prelude_mailbox_assign_owner, same protocol
 * as kai_prelude_mailbox_alloc_unowned. */
static KaiValue *kai_prelude_mailbox_alloc_bounded_unowned(KaiValue *cap, KaiValue *overflow) {
    int c = (kai_is_int(cap)) ? (int) kai_intf(cap) : 0;
    int o = (kai_is_int(overflow)) ? (int) kai_intf(overflow) : 0;
    KaiValue *r = kai_pid_value(kai_mailbox_alloc_bounded_unowned(c, o));
    /* Consume input refs — callee-consumes discipline (issue #82). */
    if (cap)      kai_decref(cap);
    if (overflow) kai_decref(overflow);
    return r;
}

static KaiValue *kai_prelude_mailbox_send(KaiValue *pid, KaiValue *msg) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_send: argument is not a Pid\n");
        exit(1);
    }
    /* m5.x flip Phase 3 closeout (issue #82): transfer the caller's
     * `msg` ref directly into the mailbox (kai_mailbox_push takes
     * ownership) and consume the `pid` ref. Pre-fix the helper did
     * `kai_incref(msg)` then dropped the caller's ref on the floor. */
    kai_mailbox_push(pid->as.mb, msg);
    kai_decref(pid);
    return kai_unit();
}

static KaiValue *kai_prelude_mailbox_recv(KaiValue *pid) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_recv: argument is not a Pid\n");
        exit(1);
    }
    /* kai_mailbox_pop returns the stored ref (mailbox transferred its
     * ownership to the caller). Consume the input `pid` ref so the
     * helper is callee-consumes-clean. */
    KaiValue *msg = kai_mailbox_pop(pid->as.mb);
    kai_decref(pid);
    return msg;
}

/* Receive within a deadline. `timeout_nanos` is a relative nanosecond
 * budget; returns `Some(msg)` if a message arrives in time, `None` if
 * the deadline elapses first. Mirrors `kai_prelude_mailbox_recv`'s
 * callee-consumes-clean discipline for the input `pid` ref. */
static KaiValue *kai_prelude_mailbox_recv_timeout(KaiValue *pid, KaiValue *timeout_nanos) {
    if (!pid || pid->tag != KAI_PID || !pid->as.mb) {
        fprintf(stderr, "kai: mailbox_recv_timeout: argument is not a Pid\n");
        exit(1);
    }
    int64_t ns = kai_intf(timeout_nanos);
    kai_decref(timeout_nanos);
    KaiValue *msg = kai_mailbox_pop_timeout(pid->as.mb,
                                            ns < 0 ? 0 : (uint64_t) ns);
    kai_decref(pid);
    if (msg) {
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

/* Free the mailbox attached to a Pid. Called by `with_mailbox` when
 * the scope exits; the Pid value itself is RC-managed independently. */
static KaiValue *kai_prelude_mailbox_free(KaiValue *pid) {
    if (pid && pid->tag == KAI_PID && pid->as.mb) {
        kai_mailbox_free(pid->as.mb);
        pid->as.mb = NULL;
    }
    /* m5.x flip Phase 3 closeout (issue #82): consume input ref. */
    if (pid) kai_decref(pid);
    return kai_unit();
}

/* ---------- prelude: file io ---------- */

static KAI_RC_NOINLINE KaiValue *kai_prelude_read_file(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("read_file: argument is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        if (!fp) {
            KaiValue *msg = kai_str("read_file: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("read_file: seek failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: tell failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("read_file: rewind failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *v = kai_alloc(KAI_STR);
                v->as.s.len = (size_t) n;
                kai_heap_charge((size_t) n + 1);
                v->as.s.bytes = (char *) malloc((size_t) n + 1);
                if (!v->as.s.bytes) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                size_t got = fread(v->as.s.bytes, 1, (size_t) n, fp);
                fclose(fp);
                v->as.s.bytes[got] = '\0';
                v->as.s.len = got;
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = v}});
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_write_file(KaiValue *path, KaiValue *content) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!content || content->tag != KAI_STR) {
        KaiValue *msg = kai_str("write_file: content is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("write_file: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            size_t wrote = fwrite(content->as.s.bytes, 1, content->as.s.len, fp);
            fclose(fp);
            if (wrote != content->as.s.len) {
                KaiValue *msg = kai_str("write_file: short write");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
            }
        }
    }
    if (path)    kai_decref(path);
    if (content) kai_decref(content);
    return r;
}

/* ---------- prelude: chunked / streaming file io (issue #771) ----------
 *
 * Five primitives behind the new `File` ops `open_read` / `read_chunk` /
 * `open_write` / `write_chunk` / `close_file`. Unlike the bulk
 * `read_file` / `write_file` pair these keep an OS file descriptor open
 * across calls, so the carrier (line surface, #801) can pull a large
 * file chunk-by-chunk without materialising it. The handle is an opaque
 * `FileHandle` record `{ fd: Int }` — same shape as the net `Conn`.
 *
 * Error register is `Result[String, _]` (Err message first), mirroring
 * `read_file`. `read_chunk` returns `Ok("")` at EOF (the spec'd
 * sentinel), never an error. Each primitive consumes its KaiValue *
 * args linearly, decref'ing before building the result.
 *
 * Stage2 boxes small Ints tagged, so the `fd` slot inside a FileHandle
 * record is read with kai_is_int / kai_intf — NEVER `->tag` / `->as.i`,
 * which segfault on a tagged 0x1. String args stay `->tag == KAI_STR`:
 * a String is always a heap pointer, never a tagged Int. */

/* Build a FileHandle record `{ fd }`. Field-name string matches the
 * kaikai-side `type FileHandle = { fd: Int }` decl
 * (builtin_filehandle_decl) — kai_op_field reads by strcmp. */
static KaiValue *_kai_file_make_handle(int fd) {
    KaiValue *fd_kv = kai_int((int64_t) fd);
    KaiValue *fields[1] = { fd_kv };
    static const char *names[1] = { "fd" };
    return kai_record(1, fields, names);
}

/* Pull the `fd` slot out of a FileHandle record. Returns -1 when the
 * value is the wrong shape (caller returns an Err). The slot is an Int,
 * which stage2 boxes tagged — read it with kai_is_int / kai_intf. */
static int _kai_file_handle_fd(KaiValue *v) {
    if (!kai_is_ptr(v) || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "fd") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

static KaiValue *_kai_file_err(const char *msg) {
    KaiValue *m = kai_str(msg);
    return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
}

static KaiValue *_kai_file_ok(KaiValue *payload) {
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = payload}});
}

/* open_read(path) -> Result[String, FileHandle]. Opens `path` read-only
 * and hands back an owning FileHandle. */
static KaiValue *kai_prelude_file_open_read(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        r = _kai_file_err("open_read: argument is not a String");
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        int fd = open(pbuf, O_RDONLY);
        if (fd < 0) r = _kai_file_err("open_read: cannot open file");
        else        r = _kai_file_ok(_kai_file_make_handle(fd));
    }
    if (path) kai_decref(path);
    return r;
}

/* read_chunk(h, max) -> Result[String, String]. Reads up to `max` bytes
 * from the handle's fd into a fresh String. `Ok("")` signals EOF. A
 * short read (fewer than `max` bytes, but more than zero) is normal and
 * returned as-is; the caller loops until it sees the empty string. */
static KaiValue *kai_prelude_file_read_chunk(KaiValue *h, KaiValue *max) {
    KaiValue *r = NULL;
    int fd = _kai_file_handle_fd(h);
    int64_t cap = kai_is_int(max) ? kai_intf(max) : -1;
    if (fd < 0) {
        r = _kai_file_err("read_chunk: bad handle");
    } else if (cap < 0) {
        r = _kai_file_err("read_chunk: max is not a non-negative Int");
    } else if (cap == 0) {
        r = _kai_file_ok(kai_str(""));
    } else {
        char *buf = (char *) malloc((size_t) cap);
        if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
        ssize_t got;
        do { got = read(fd, buf, (size_t) cap); } while (got < 0 && errno == EINTR);
        if (got < 0) {
            free(buf);
            r = _kai_file_err("read_chunk: read failed");
        } else {
            KaiValue *v = kai_alloc(KAI_STR);
            v->as.s.len = (size_t) got;
            kai_heap_charge((size_t) got + 1);
            v->as.s.bytes = (char *) malloc((size_t) got + 1);
            if (!v->as.s.bytes) { free(buf); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            memcpy(v->as.s.bytes, buf, (size_t) got);
            v->as.s.bytes[got] = '\0';
            free(buf);
            r = _kai_file_ok(v);
        }
    }
    if (kai_is_ptr(h))   kai_decref(h);
    if (kai_is_ptr(max)) kai_decref(max);
    return r;
}

/* open_write(path) -> Result[String, FileHandle]. Opens `path` for
 * writing, creating it (mode 0644) and truncating any existing
 * contents, then hands back an owning FileHandle. */
static KaiValue *kai_prelude_file_open_write(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        r = _kai_file_err("open_write: argument is not a String");
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        int fd = open(pbuf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) r = _kai_file_err("open_write: cannot open file");
        else        r = _kai_file_ok(_kai_file_make_handle(fd));
    }
    if (path) kai_decref(path);
    return r;
}

/* write_chunk(h, data) -> Result[String, Unit]. Writes every byte of
 * `data` to the handle's fd, looping over partial writes. */
static KaiValue *kai_prelude_file_write_chunk(KaiValue *h, KaiValue *data) {
    KaiValue *r = NULL;
    int fd = _kai_file_handle_fd(h);
    if (fd < 0) {
        r = _kai_file_err("write_chunk: bad handle");
    } else if (!data || data->tag != KAI_STR) {
        r = _kai_file_err("write_chunk: data is not a String");
    } else {
        size_t total = data->as.s.len;
        size_t off = 0;
        int failed = 0;
        while (off < total) {
            ssize_t w = write(fd, data->as.s.bytes + off, total - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                failed = 1;
                break;
            }
            off += (size_t) w;
        }
        if (failed) r = _kai_file_err("write_chunk: write failed");
        else        r = _kai_file_ok(kai_unit());
    }
    if (kai_is_ptr(h))    kai_decref(h);
    if (kai_is_ptr(data)) kai_decref(data);
    return r;
}

/* close_file(h) -> Unit. Closes the underlying fd; a bad handle or a
 * close error is swallowed (there is no Result register on close — the
 * fd is gone either way). */
static KaiValue *kai_prelude_file_close(KaiValue *h) {
    int fd = _kai_file_handle_fd(h);
    if (fd >= 0) close(fd);
    if (kai_is_ptr(h)) kai_decref(h);
    return kai_unit();
}

/* Issue #345: file_exists / file_delete / file_rename. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * matching the prelude convention used by read_file/write_file above. */

static KaiValue *kai_prelude_file_exists(KaiValue *path) {
    int present = 0;
    if (kai_is_ptr(path) && path->tag == KAI_STR) {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        present = (access(pbuf, F_OK) == 0) ? 1 : 0;
    }
    if (path) kai_decref(path);
    return kai_bool(present);
}

static KaiValue *kai_prelude_file_delete(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_delete: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (unlink(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("file_delete: unlink failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_file_rename(KaiValue *from, KaiValue *to) {
    KaiValue *r = NULL;
    if (!from || from->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: from is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!to || to->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: to is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char fbuf[KAI_PATH_BUF];
        char tbuf[KAI_PATH_BUF];
        size_t flen = from->as.s.len < sizeof(fbuf) - 1 ? from->as.s.len : sizeof(fbuf) - 1;
        size_t tlen = to->as.s.len   < sizeof(tbuf) - 1 ? to->as.s.len   : sizeof(tbuf) - 1;
        memcpy(fbuf, from->as.s.bytes, flen); fbuf[flen] = '\0';
        memcpy(tbuf, to->as.s.bytes,   tlen); tbuf[tlen] = '\0';
        if (rename(fbuf, tbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("file_rename: rename failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (from) kai_decref(from);
    if (to)   kai_decref(to);
    return r;
}

/* Issue #482 (follow-up to #345) + Array[Byte] refactor (prereq for
 * #452): binary file IO. Operates on `Array[Byte]` so the buffer
 * lines up with BinSerialize post-#488 (O(1) reads, contiguous
 * storage). Both primitives consume their args linearly (kai_decref
 * before allocating the result), matching the convention used by the
 * text variants. */

static KaiValue *kai_prelude_file_read_bytes(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_read_bytes: argument is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        if (!fp) {
            KaiValue *msg = kai_str("file_read_bytes: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("file_read_bytes: seek failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: tell failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: rewind failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                /* Read into a flat C buffer, then publish into a
                 * fresh KAI_ARRAY one KAI_BYTE allocation per slot.
                 * Allocating the kai_array directly (instead of
                 * kai_array_make + array_set in a loop) avoids the
                 * redundant default-incref/decref pair on every
                 * position. */
                unsigned char *buf = (unsigned char *) malloc((size_t) n + 1);
                if (!buf) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                size_t got = fread(buf, 1, (size_t) n, fp);
                fclose(fp);
                KaiValue *arr = kai_alloc(KAI_ARRAY);
                arr->as.arr.len = (int64_t) got;
                arr->as.arr.cap = got > 0 ? (int64_t) got : 1;
                kai_heap_charge((size_t) arr->as.arr.cap * sizeof(KaiValue *));
                arr->as.arr.items = (KaiValue **) malloc((size_t) arr->as.arr.cap * sizeof(KaiValue *));
                if (!arr->as.arr.items) { free(buf); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                for (size_t i = 0; i < got; ++i) arr->as.arr.items[i] = kai_byte(buf[i]);
                free(buf);
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = arr}});
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_file_write_bytes(KaiValue *path, KaiValue *bytes) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_write_bytes: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!bytes || bytes->tag != KAI_ARRAY) {
        KaiValue *msg = kai_str("file_write_bytes: buffer is not an Array");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("file_write_bytes: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            /* Pack the kai_array into a contiguous C buffer, then
             * issue a single fwrite. Cheaper than per-slot fwrite
             * even with stdio buffering, and lets us fail fast on
             * a non-Byte slot before any IO. */
            int64_t n = bytes->as.arr.len;
            int ok = 1;
            unsigned char *out = NULL;
            if (n > 0) {
                out = (unsigned char *) malloc((size_t) n);
                if (!out) { fclose(fp); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                for (int64_t i = 0; i < n; ++i) {
                    KaiValue *e = bytes->as.arr.items[i];
                    if (!e || e->tag != KAI_BYTE) { ok = 0; break; }
                    out[i] = e->as.byte_val;
                }
            }
            if (ok && n > 0) {
                if (fwrite(out, 1, (size_t) n, fp) != (size_t) n) ok = 0;
            }
            if (out) free(out);
            fclose(fp);
            if (!ok) {
                KaiValue *msg = kai_str("file_write_bytes: write failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
            }
        }
    }
    if (path)  kai_decref(path);
    if (bytes) kai_decref(bytes);
    return r;
}

/* Issue #344: directory ops on top of the `File` effect. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * mirroring the file_exists/_delete/_rename convention. POSIX only
 * (macOS + Linux). Return shapes:
 *
 *   dir_list_dir(path)    : [String]              — entries (no . / ..)
 *   dir_create_dir(path)  : Result[String, Unit]  — Err message first
 *   dir_remove_dir(path)  : Result[String, Unit]
 *   dir_walk(path)        : [String]              — files (not dirs),
 *                                                   depth-first; symlinks
 *                                                   are NOT followed in v1
 *
 * dir_list_dir returns the empty list on read errors (matching how
 * args() can be empty); use dir_create_dir / dir_remove_dir when an
 * explicit Result is wanted. */

static KaiValue *kai_prelude_dir_list_dir(KaiValue *path) {
    KaiValue *acc = kai_nil();
    if (kai_is_ptr(path) && path->tag == KAI_STR) {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        DIR *d = opendir(pbuf);
        if (d) {
            /* Buffer entries then prepend in reverse so the caller sees
             * them in readdir order. readdir order is filesystem-defined
             * but stable for a given mount, which keeps test output
             * deterministic enough for fixtures. */
            size_t cap = 16, n = 0;
            char **names = (char **) malloc(cap * sizeof(char *));
            if (!names) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *nm = e->d_name;
                if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
                if (n + 1 > cap) {
                    cap *= 2;
                    names = (char **) realloc(names, cap * sizeof(char *));
                    if (!names) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                size_t len = strlen(nm);
                char *copy = (char *) malloc(len + 1);
                if (!copy) { closedir(d); fprintf(stderr, "kai: out of memory\n"); exit(1); }
                memcpy(copy, nm, len + 1);
                names[n++] = copy;
            }
            closedir(d);
            for (size_t i = n; i > 0;) {
                --i;
                acc = kai_cons(kai_str(names[i]), acc);
                free(names[i]);
            }
            free(names);
        }
    }
    if (path) kai_decref(path);
    return acc;
}

static KaiValue *kai_prelude_dir_create_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_create_dir: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (mkdir(pbuf, KAI_DIR_MODE) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("dir_create_dir: mkdir failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_prelude_dir_remove_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_remove_dir: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (rmdir(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("dir_remove_dir: rmdir failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

/* Iterative depth-first walk. Push subdirectories onto an explicit stack
 * so a deep tree doesn't blow the C stack. Symlinks are NOT followed
 * (v1 contract): use lstat + S_ISLNK skip so a symlink loop can't
 * trap the walker. Only regular files are emitted; directories are
 * traversed but not reported. */
static KaiValue *kai_prelude_dir_walk(KaiValue *root) {
    KaiValue *acc = kai_nil();
    if (!root || root->tag != KAI_STR) {
        if (root) kai_decref(root);
        return acc;
    }

    size_t cap = 16, top = 0;
    char **stack = (char **) malloc(cap * sizeof(char *));
    if (!stack) { fprintf(stderr, "kai: out of memory\n"); exit(1); }

    char rbuf[KAI_PATH_BUF];
    size_t rlen = root->as.s.len < sizeof(rbuf) - 1 ? root->as.s.len : sizeof(rbuf) - 1;
    memcpy(rbuf, root->as.s.bytes, rlen);
    rbuf[rlen] = '\0';
    {
        char *copy = (char *) malloc(rlen + 1);
        if (!copy) { free(stack); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        memcpy(copy, rbuf, rlen + 1);
        stack[top++] = copy;
    }

    /* Collect files first, then prepend in reverse so the cons list
     * reads in walk order. */
    size_t fcap = 32, fn = 0;
    char **files = (char **) malloc(fcap * sizeof(char *));
    if (!files) { free(stack[0]); free(stack); fprintf(stderr, "kai: out of memory\n"); exit(1); }

    while (top > 0) {
        char *dir_path = stack[--top];
        DIR *d = opendir(dir_path);
        if (!d) { free(dir_path); continue; }
        /* Buffer child entries so we can push subdirs in reverse for
         * deterministic depth-first order. */
        size_t ccap = 16, cn = 0;
        char **child_paths = (char **) malloc(ccap * sizeof(char *));
        if (!child_paths) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        char *child_kinds = (char *) malloc(ccap); /* 'f' / 'd' / 's' (skip) */
        if (!child_kinds) { free(child_paths); closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
            size_t dlen = strlen(dir_path);
            size_t nlen = strlen(nm);
            char *full = (char *) malloc(dlen + 1 + nlen + 1);
            if (!full) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            memcpy(full, dir_path, dlen);
            int needs_sep = (dlen > 0 && dir_path[dlen - 1] != '/');
            size_t off = dlen;
            if (needs_sep) { full[off++] = '/'; }
            memcpy(full + off, nm, nlen + 1);
            struct stat st;
            char kind = 's';
            if (lstat(full, &st) == 0) {
                if (S_ISLNK(st.st_mode))      kind = 's'; /* skip symlinks */
                else if (S_ISDIR(st.st_mode)) kind = 'd';
                else if (S_ISREG(st.st_mode)) kind = 'f';
                else                          kind = 's'; /* sockets, fifos, etc. */
            }
            if (cn + 1 > ccap) {
                ccap *= 2;
                child_paths = (char **) realloc(child_paths, ccap * sizeof(char *));
                child_kinds = (char *)  realloc(child_kinds, ccap);
                if (!child_paths || !child_kinds) { closedir(d); free(dir_path); fprintf(stderr, "kai: out of memory\n"); exit(1); }
            }
            child_paths[cn] = full;
            child_kinds[cn] = kind;
            cn++;
        }
        closedir(d);
        free(dir_path);
        /* Emit files in encountered order. */
        for (size_t i = 0; i < cn; ++i) {
            char k = child_kinds[i];
            if (k == 'f') {
                if (fn + 1 > fcap) {
                    fcap *= 2;
                    files = (char **) realloc(files, fcap * sizeof(char *));
                    if (!files) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                files[fn++] = child_paths[i];
            }
        }
        /* Push subdirs in reverse onto the stack so they pop in natural order. */
        for (size_t i = cn; i > 0;) {
            --i;
            char k = child_kinds[i];
            if (k == 'd') {
                if (top + 1 > cap) {
                    cap *= 2;
                    stack = (char **) realloc(stack, cap * sizeof(char *));
                    if (!stack) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
                }
                stack[top++] = child_paths[i];
            } else if (k != 'f') {
                /* skipped — free the path string */
                free(child_paths[i]);
            }
        }
        free(child_paths);
        free(child_kinds);
    }
    free(stack);

    /* Build the cons list in reverse so consumers see files in walk order. */
    for (size_t i = fn; i > 0;) {
        --i;
        acc = kai_cons(kai_str(files[i]), acc);
        free(files[i]);
    }
    free(files);

    kai_decref(root);
    return acc;
}

static KaiValue *kai_prelude_read_line(void) {
    size_t cap = KAI_READ_BUF_INIT, n = 0;
    char *buf = (char *) malloc(cap);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    int ch;
    while ((ch = fgetc(stdin)) != EOF && ch != '\n') {
        if (n + 1 >= cap) { cap *= 2; buf = (char *) realloc(buf, cap); }
        buf[n++] = (char) ch;
    }
    if (ch == EOF && n == 0) {
        free(buf);
        KaiValue *msg = kai_str("read_line: end of input");
        return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    }
    KaiValue *s = kai_str_from_bytes(buf, n);
    free(buf);
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = s}});
}

/* Issue #453 + #620: byte-oriented stdin read. Reads up to `n` raw
 * bytes from stdin (including '\n') and returns them as a String.
 * On EOF the returned String is shorter than `n` — possibly empty.
 * Used by LSP-style framed protocols where the body length is known
 * up front and may contain newlines.
 *
 * R3 unification: this prelude entry and the `Stdin.read_bytes`
 * default handler both go through `read(STDIN_FILENO, …)` so that a
 * program mixing the two forms (flat `read_bytes(n)` and qualified
 * `Stdin.read_bytes(n)`) consumes the input stream byte-for-byte
 * without libc's stdio buffer in the middle. The flat prelude path
 * stays blocking (it predates the reactor and is reachable from
 * stage 0 binaries that have no fibers); the qualified handler
 * parks the fiber on EAGAIN. */
static KaiValue *kai_prelude_read_bytes(KaiValue *n) {
    int64_t want = 0;
    if (n && kai_is_int(n) && kai_intf(n) > 0) want = kai_intf(n);
    if (n) kai_decref(n);
    if (want <= 0) return kai_str_from_bytes("", 0);
    char *buf = (char *) malloc((size_t) want);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t got = 0;
    while (got < (size_t) want) {
        ssize_t r = read(STDIN_FILENO, buf + got, (size_t) want - got);
        if (r > 0) {
            got += (size_t) r;
        } else if (r == 0) {
            break;  /* EOF */
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* fd 0 is O_NONBLOCK because the R3 reactor handler
             * flipped it earlier in this process. The flat prelude
             * has no fiber to park; emulate blocking semantics by
             * polling until the fd is readable, then retry. */
            struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
            int prc = poll(&p, 1, -1);
            if (prc < 0 && errno != EINTR) break;
            continue;
        } else {
            break;  /* I/O error — surface as short read */
        }
    }
    KaiValue *s = kai_str_from_bytes(buf, got);
    free(buf);
    return s;
}

/* ---------- prelude: parsing and string helpers ---------- */

/* m5.x flip Phase 3 closeout (Perceus Tier 2 audit, 2026-04-29):
 * the next 8 helpers consume their args linearly. Result computed
 * first, args decref'd before the constructor allocates. */
static KaiValue *kai_prelude_string_to_int(KaiValue *s) {
    int ok = 0;
    int64_t value = 0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0 && s->as.s.len < 64) {
        char buf[64];
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        long long v = strtoll(buf, &end, 10);
        if (end && *end == '\0' && end != buf) {
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

static KaiValue *kai_prelude_string_to_real(KaiValue *s) {
    int ok = 0;
    double value = 0.0;
    if (s && s->tag == KAI_STR && s->as.s.len > 0 && s->as.s.len < 64) {
        char buf[64];
        memcpy(buf, s->as.s.bytes, s->as.s.len);
        buf[s->as.s.len] = '\0';
        char *end = NULL;
        double v = strtod(buf, &end);
        if (end && *end == '\0' && end != buf) {
            value = v;
            ok = 1;
        }
    }
    if (s) kai_decref(s);
    if (ok) {
        KaiValue *rv = kai_real(value);
        return kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = rv}});
    }
    return kai_variant_u(1, "None", 0, 0, NULL);
}

static KaiValue *kai_prelude_char_at(KaiValue *s, KaiValue *i) {
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
static KaiValue *kai_prelude_string_cp_len(KaiValue *s, KaiValue *off) {
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
static KaiValue *kai_prelude_string_cp_at(KaiValue *s, KaiValue *off) {
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
static KaiValue *kai_prelude_string_reverse(KaiValue *s) {
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

static KaiValue *kai_prelude_string_split(KaiValue *s, KaiValue *sep) {
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
        size_t i = 0;
        size_t last = 0;
        while (i + seplen <= slen) {
            if (memcmp(p + i, sp, seplen) == 0) {
                if (n == cap) { cap *= 2; pieces = realloc(pieces, cap * sizeof(*pieces)); }
                pieces[n].b = p + last;
                pieces[n].l = i - last;
                n++;
                i += seplen;
                last = i;
            } else {
                i++;
            }
        }
        if (n == cap) { cap *= 2; pieces = realloc(pieces, cap * sizeof(*pieces)); }
        pieces[n].b = p + last;
        pieces[n].l = slen - last;
        n++;
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

static KaiValue *kai_prelude_string_slice(KaiValue *s, KaiValue *from, KaiValue *len) {
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

static KaiValue *kai_prelude_char_to_int(KaiValue *c) {
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
static KaiValue *kai_prelude_int_to_char(KaiValue *n) {
    int64_t cp = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (n) kai_decref(n);
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "int_to_char: %lld is not a Unicode scalar value "
                 "(0..0x10FFFF excluding surrogates)", (long long) cp);
        kai_prelude_panic(kai_str(msg));
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
static KaiValue *kai_prelude_int_to_byte_string(KaiValue *n) {
    int64_t v = (kai_is_int(n)) ? kai_intf(n) : 0;
    if (n) kai_decref(n);
    unsigned char b = (unsigned char) (v & 0xff);
    return kai_str_from_bytes((const char *) &b, 1);
}

/* Read one byte at index `i` of String `s`, returning it as an Int
 * 0..255. Returns -1 on out-of-bounds or wrong type. Faster than
 * `match char_at(s, i) { Some(c) -> char_to_int(c); None -> -1 }`
 * because it avoids the Option allocation + Char alloc + decref
 * chain (KAI_CHAR is cached for value < 128 but still hits a load
 * + tag check). The KAB2 cache decoder (#592) calls this ~1M times
 * per warm load; replacing Option-wrapped char_at cuts decoder wall
 * from ~0.26s to under 0.04s. */
static KaiValue *kai_prelude_string_byte_at_int(KaiValue *s, KaiValue *i) {
    int64_t v = -1;
    if (s && s->tag == KAI_STR && i && kai_is_int(i)) {
        int64_t idx = kai_intf(i);
        if (idx >= 0 && (size_t) idx < s->as.s.len) {
            v = (int64_t)(unsigned char) s->as.s.bytes[idx];
        }
    }
    if (s) kai_decref(s);
    if (i) kai_decref(i);
    return kai_int(v);
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
static KaiValue *kai_prelude_string_hash(KaiValue *s) {
    uint64_t h = 1469598103934665603ull;
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        const char *bytes = s->as.s.bytes;
        size_t len = s->as.s.len;
        for (size_t i = 0; i < len; i++) {
            h ^= (uint64_t) (uint8_t) bytes[i];
            h *= 1099511628211ull;
        }
    }
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
static KaiValue *kai_prelude_real_bits(KaiValue *v) {
    uint64_t bits = 0;
    if (kai_is_ptr(v) && v->tag == KAI_REAL) {
        double r = v->as.r;
        memcpy(&bits, &r, sizeof(bits));
    }
    if (v) kai_decref(v);
    return kai_int((int64_t) bits);
}

static KaiValue *kai_prelude_string_contains(KaiValue *s, KaiValue *sub) {
    int yes = 0;
    if (s && s->tag == KAI_STR && sub && sub->tag == KAI_STR) {
        if (sub->as.s.len == 0) {
            yes = 1;
        } else if (sub->as.s.len <= s->as.s.len) {
            for (size_t i = 0; i + sub->as.s.len <= s->as.s.len; ++i) {
                if (memcmp(s->as.s.bytes + i, sub->as.s.bytes, sub->as.s.len) == 0) {
                    yes = 1;
                    break;
                }
            }
        }
    }
    if (s)   kai_decref(s);
    if (sub) kai_decref(sub);
    return kai_bool(yes);
}

/* ---------- prelude thunks for first-class function refs ---------- */

static KaiValue *_kai_prelude_print_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_print(a[0]); }
static KaiValue *_kai_prelude_eprint_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_eprint(a[0]); }
static KaiValue *_kai_prelude_write_stdout_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_write_stdout(a[0]); }
static KaiValue *_kai_prelude_panic_thunk(KaiValue *s, KaiValue **a, int n)          { (void) s; (void) n; return kai_prelude_panic(a[0]); }
static KaiValue *_kai_prelude_exit_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_exit(a[0]); }
static KaiValue *_kai_prelude_int_to_string_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_int_to_string(a[0]); }
static KaiValue *_kai_prelude_real_to_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_real_to_string(a[0]); }
static KaiValue *_kai_prelude_int_to_real_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_int_to_real(a[0]); }
static KaiValue *_kai_prelude_real_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_to_int(a[0]); }
static KaiValue *_kai_prelude_real_sqrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_sqrt(a[0]); }
static KaiValue *_kai_prelude_real_cbrt_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_cbrt(a[0]); }
static KaiValue *_kai_prelude_real_exp_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_exp(a[0]); }
static KaiValue *_kai_prelude_real_log_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_log(a[0]); }
static KaiValue *_kai_prelude_real_log2_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_log2(a[0]); }
static KaiValue *_kai_prelude_real_log10_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_real_log10(a[0]); }
static KaiValue *_kai_prelude_real_sin_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_sin(a[0]); }
static KaiValue *_kai_prelude_real_cos_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_cos(a[0]); }
static KaiValue *_kai_prelude_real_tan_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_tan(a[0]); }
static KaiValue *_kai_prelude_real_asin_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_asin(a[0]); }
static KaiValue *_kai_prelude_real_acos_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_acos(a[0]); }
static KaiValue *_kai_prelude_real_atan_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_atan(a[0]); }
static KaiValue *_kai_prelude_real_sinh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_sinh(a[0]); }
static KaiValue *_kai_prelude_real_cosh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_cosh(a[0]); }
static KaiValue *_kai_prelude_real_tanh_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_real_tanh(a[0]); }
static KaiValue *_kai_prelude_real_signum_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_signum(a[0]); }
static KaiValue *_kai_prelude_real_is_nan_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_is_nan(a[0]); }
static KaiValue *_kai_prelude_real_is_inf_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_real_is_inf(a[0]); }
static KaiValue *_kai_prelude_real_pow_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_pow(a[0], a[1]); }
static KaiValue *_kai_prelude_real_atan2_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_real_atan2(a[0], a[1]); }
static KaiValue *_kai_prelude_real_rem_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_real_rem(a[0], a[1]); }
static KaiValue *_kai_prelude_string_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_length(a[0]); }
static KaiValue *_kai_prelude_string_concat_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_concat(a[0], a[1]); }
static KaiValue *_kai_prelude_string_concat_all_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_concat_all(a[0]); }
static KaiValue *_kai_prelude_string_join_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_join(a[0], a[1]); }
static KaiValue *_kai_prelude_array_make_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_make(a[0], a[1]); }
static KaiValue *_kai_prelude_array_length_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_array_length(a[0]); }
static KaiValue *_kai_prelude_array_get_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_get(a[0], a[1]); }
static KaiValue *_kai_prelude_array_set_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_array_set(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_array_grow_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_array_grow(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_ref_make_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_ref_make(a[0]); }
static KaiValue *_kai_prelude_ref_get_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_ref_get(a[0]); }
static KaiValue *_kai_prelude_ref_set_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_ref_set(a[0], a[1]); }
static KaiValue *_kai_prelude_list_length_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_length(a[0]); }
static KaiValue *_kai_prelude_list_append_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_list_append(a[0], a[1]); }
static KaiValue *_kai_prelude_list_reverse_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_list_reverse(a[0]); }
static KaiValue *_kai_prelude_map_thunk(KaiValue *s, KaiValue **a, int n)            { (void) s; (void) n; return kai_prelude_map(a[0], a[1]); }
static KaiValue *_kai_prelude_flat_map_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_flat_map(a[0], a[1]); }
static KaiValue *_kai_prelude_filter_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_filter(a[0], a[1]); }
static KaiValue *_kai_prelude_reduce_thunk(KaiValue *s, KaiValue **a, int n)         { (void) s; (void) n; return kai_prelude_reduce(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_each_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) n; return kai_prelude_each(a[0], a[1]); }
static KaiValue *_kai_prelude_args_thunk(KaiValue *s, KaiValue **a, int n)           { (void) s; (void) a; (void) n; return kai_prelude_args(); }
static KaiValue *_kai_prelude_program_name_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) a; (void) n; return kai_prelude_program_name(); }
static KaiValue *_kai_prelude_stdlib_path_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) a; (void) n; return kai_prelude_stdlib_path(); }
static KaiValue *_kai_prelude_abspath_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_prelude_abspath(a[0]); }
static KaiValue *_kai_prelude_read_file_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) n; return kai_prelude_read_file(a[0]); }
static KaiValue *_kai_prelude_write_file_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_write_file(a[0], a[1]); }
static KaiValue *_kai_prelude_file_exists_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_exists(a[0]); }
static KaiValue *_kai_prelude_file_delete_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_delete(a[0]); }
static KaiValue *_kai_prelude_file_rename_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_file_rename(a[0], a[1]); }
static KaiValue *_kai_prelude_file_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_file_read_bytes(a[0]); }
static KaiValue *_kai_prelude_file_write_bytes_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_file_write_bytes(a[0], a[1]); }
static KaiValue *_kai_prelude_dir_list_dir_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_dir_list_dir(a[0]); }
static KaiValue *_kai_prelude_dir_create_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_dir_create_dir(a[0]); }
static KaiValue *_kai_prelude_dir_remove_dir_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_dir_remove_dir(a[0]); }
static KaiValue *_kai_prelude_dir_walk_thunk(KaiValue *s, KaiValue **a, int n)       { (void) s; (void) n; return kai_prelude_dir_walk(a[0]); }
static KaiValue *_kai_prelude_read_line_thunk(KaiValue *s, KaiValue **a, int n)      { (void) s; (void) a; (void) n; return kai_prelude_read_line(); }
static KaiValue *_kai_prelude_read_bytes_thunk(KaiValue *s, KaiValue **a, int n)     { (void) s; (void) n; return kai_prelude_read_bytes(a[0]); }
static KaiValue *_kai_prelude_string_to_int_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_to_int(a[0]); }
static KaiValue *_kai_prelude_string_to_real_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_to_real(a[0]); }
static KaiValue *_kai_prelude_char_at_thunk(KaiValue *s, KaiValue **a, int n)        { (void) s; (void) n; return kai_prelude_char_at(a[0], a[1]); }
static KaiValue *_kai_prelude_string_split_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_split(a[0], a[1]); }
static KaiValue *_kai_prelude_string_contains_thunk(KaiValue *s, KaiValue **a, int n){ (void) s; (void) n; return kai_prelude_string_contains(a[0], a[1]); }
static KaiValue *_kai_prelude_string_slice_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_string_slice(a[0], a[1], a[2]); }
static KaiValue *_kai_prelude_char_to_int_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_char_to_int(a[0]); }
static KaiValue *_kai_prelude_int_to_char_thunk(KaiValue *s, KaiValue **a, int n)    { (void) s; (void) n; return kai_prelude_int_to_char(a[0]); }
static KaiValue *_kai_prelude_int_to_byte_string_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_int_to_byte_string(a[0]); }
static KaiValue *_kai_prelude_string_byte_at_int_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_byte_at_int(a[0], a[1]); }
static KaiValue *_kai_prelude_string_cp_at_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) n; return kai_prelude_string_cp_at(a[0], a[1]); }
static KaiValue *_kai_prelude_string_cp_len_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_cp_len(a[0], a[1]); }
static KaiValue *_kai_prelude_string_reverse_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_reverse(a[0]); }
static KaiValue *_kai_prelude_string_hash_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_string_hash(a[0]); }
static KaiValue *_kai_prelude_real_bits_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_real_bits(a[0]); }
static KaiValue *_kai_prelude_mailbox_alloc_thunk(KaiValue *s, KaiValue **a, int n)  { (void) s; (void) a; (void) n; return kai_prelude_mailbox_alloc(); }
static KaiValue *_kai_prelude_mailbox_alloc_bounded_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_mailbox_alloc_bounded(a[0], a[1]); }
static KaiValue *_kai_prelude_mailbox_send_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_send(a[0], a[1]); }
static KaiValue *_kai_prelude_mailbox_recv_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_recv(a[0]); }
static KaiValue *_kai_prelude_mailbox_recv_timeout_thunk(KaiValue *s, KaiValue **a, int n) { (void) s; (void) n; return kai_prelude_mailbox_recv_timeout(a[0], a[1]); }
static KaiValue *_kai_prelude_mailbox_free_thunk(KaiValue *s, KaiValue **a, int n)   { (void) s; (void) n; return kai_prelude_mailbox_free(a[0]); }

/* ---------- test harness hooks (used by --test runs) ---------- */

static int         kai_test_count_total  = 0;
static int         kai_test_count_passed = 0;
static const char *kai_test_current      = NULL;
static jmp_buf     kai_test_jmp;
static int         kai_test_in_progress  = 0;

static void kai_test_begin(const char *desc) {
    kai_test_count_total++;
    kai_test_current = desc;
}

static void kai_test_pass(void) {
    kai_test_count_passed++;
    fprintf(stderr, "  ok   %s\n", kai_test_current ? kai_test_current : "");
}

static void kai_test_fail(const char *desc, const char *msg) {
    fprintf(stderr, "  FAIL %s : %s\n",
            desc ? desc : "",
            msg  ? msg  : "assertion failed");
}

static int kai_test_summary(void) {
    fprintf(stderr, "\n%d/%d tests passed\n",
            kai_test_count_passed, kai_test_count_total);
    return (kai_test_count_passed == kai_test_count_total) ? 0 : 1;
}

/* Run one test body through the begin/setjmp/pass landing pad. The
   C-direct backend weaves this same shape inline into each emitted
   `_kai_test_<id>`; the native backend cannot emit a generated `int
   main`, so it emits each test body as a plain fn and drives them
   through this helper. The setjmp landing lives HERE, not in `body` —
   so `kai_assert_check`'s longjmp on a failed assertion unwinds into a
   frame still on the stack, and the body fn itself stays
   mem2reg-promotable (no setjmp in its IR). `body` returns the block's
   final value (a boxed `KaiValue *`), decref'd here exactly as the
   C-direct runner's `kai_decref(_body)`. */
static void kai_test_run_one(const char *desc, KaiValue *(*body)(void)) {
    kai_test_begin(desc);
    if (setjmp(kai_test_jmp) == 0) {
        kai_test_in_progress = 1;
        KaiValue *r = body();
        kai_decref(r);
        kai_test_in_progress = 0;
        kai_test_pass();
    } else {
        kai_test_in_progress = 0;
    }
}

/* ---------- bench harness hooks (used by --bench runs) ----------
 * bench v1.x (issue #437): per-iteration timings collected into a
 * sample buffer; on finalize we sort and report median + MAD + mean
 * + range. The emitted bench wrapper runs KAI_BENCH_WARMUP untimed
 * iterations first to take JIT/cache effects out of the timed
 * window, then KAI_BENCH_ITERS timed iterations. Both knobs read
 * from getenv() at startup so the user can override without
 * recompiling. KAI_BENCH_ITERS_DEFAULT is the compile-time fallback.
 *
 * Selfhost-bench (the compiler measuring itself) deferred to v1.y;
 * see issue #40 for the split plan.
 */

#define KAI_BENCH_ITERS_DEFAULT  1000
#define KAI_BENCH_WARMUP_DEFAULT 50

static int kai_bench_count_total = 0;

static int kai_bench_iters_cached = -1;
static int kai_bench_warmup_cached = -1;

static int kai_bench_parse_int_env(const char *name, int fallback) {
    const char *raw = getenv(name);
    if (!raw || !*raw) return fallback;
    long v = 0;
    const char *p = raw;
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    if (!*p) return fallback;
    while (*p) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
        if (v > 100000000L) return fallback;
        p++;
    }
    v *= sign;
    if (v < 0) return fallback;
    return (int)v;
}

static int kai_bench_iters(void) {
    if (kai_bench_iters_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_ITERS", KAI_BENCH_ITERS_DEFAULT);
        if (v < 1) v = 1;
        kai_bench_iters_cached = v;
    }
    return kai_bench_iters_cached;
}

static int kai_bench_warmup(void) {
    if (kai_bench_warmup_cached < 0) {
        int v = kai_bench_parse_int_env("KAI_BENCH_WARMUP", KAI_BENCH_WARMUP_DEFAULT);
        if (v < 0) v = 0;
        kai_bench_warmup_cached = v;
    }
    return kai_bench_warmup_cached;
}

static long long kai_bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
}

/* Per-bench sample buffer. One bench at a time runs to completion
   before the next one starts (the emitted main calls them in
   sequence), so a single shared buffer is enough — we just realloc
   it lazily to the high-water mark. */
static long long *kai_bench_samples = NULL;
static int kai_bench_samples_cap = 0;

static void kai_bench_ensure_capacity(int n) {
    if (n <= kai_bench_samples_cap) return;
    long long *r = (long long *)realloc(kai_bench_samples, (size_t)n * sizeof(long long));
    if (!r) {
        fprintf(stderr, "kai_bench: out of memory reserving %d samples\n", n);
        exit(1);
    }
    kai_bench_samples = r;
    kai_bench_samples_cap = n;
}

static void kai_bench_record(int idx, long long sample_ns) {
    /* Caller has already ensured capacity. Out-of-range index
       silently drops — defensive against emitter bugs that miscount
       iterations. */
    if (idx < 0 || idx >= kai_bench_samples_cap) return;
    kai_bench_samples[idx] = sample_ns;
}

static int kai_bench_ll_cmp(const void *a, const void *b) {
    long long la = *(const long long *)a;
    long long lb = *(const long long *)b;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

static long long kai_bench_median_sorted(const long long *xs, int n) {
    if (n <= 0) return 0;
    return xs[n / 2];
}

static void kai_bench_finalize(const char *desc, int iters) {
    /* Samples already populated by kai_bench_record(0..iters-1). */
    if (iters <= 0) {
        fprintf(stderr, "  %s: 0 iter / median 0 ns / MAD 0 ns / mean 0 ns / range [0, 0]\n",
                desc ? desc : "(unnamed)");
        kai_bench_count_total++;
        return;
    }
    long long *xs = kai_bench_samples;
    long long total = 0;
    long long mn = xs[0];
    long long mx = xs[0];
    for (int i = 0; i < iters; i++) {
        long long s = xs[i];
        total += s;
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    long long mean = total / (long long)iters;
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long median = kai_bench_median_sorted(xs, iters);
    /* MAD: median(|x_i - median|). Reuse the sample buffer for the
       deviations — we're done with the sorted samples. */
    for (int i = 0; i < iters; i++) {
        long long d = xs[i] - median;
        if (d < 0) d = -d;
        xs[i] = d;
    }
    qsort(xs, (size_t)iters, sizeof(long long), kai_bench_ll_cmp);
    long long mad = kai_bench_median_sorted(xs, iters);
    fprintf(stderr,
            "  %s: %d iter / median %lld ns / MAD %lld ns / mean %lld ns / range [%lld, %lld]\n",
            desc ? desc : "(unnamed)", iters, median, mad, mean, mn, mx);
    kai_bench_count_total++;
}

static int kai_bench_summary(void) {
    fprintf(stderr, "\n%d benches\n", kai_bench_count_total);
    if (kai_bench_samples) {
        free(kai_bench_samples);
        kai_bench_samples = NULL;
        kai_bench_samples_cap = 0;
    }
    return 0;
}

/* Run one bench body through warmup + timed iterations. The C-direct
   backend weaves this loop inline into each `_kai_bench_<id>`; the
   native backend emits each body as a fn returning the block's final
   boxed value and drives them here, decref'ing each result like the
   C-direct runner. No setjmp pad — an assertion inside a bench should
   panic, since timing aborted code is meaningless (`kai_assert_check`
   with `kai_test_in_progress == 0` panics, which is the wanted
   behaviour). */
static void kai_bench_run_one(const char *desc, KaiValue *(*body)(void)) {
    int iters  = kai_bench_iters();
    int warmup = kai_bench_warmup();
    int i;
    kai_bench_ensure_capacity(iters);
    for (i = 0; i < warmup; i++) kai_decref(body());
    for (i = 0; i < iters; i++) {
        long long a = kai_bench_now_ns();
        KaiValue *r = body();
        long long b = kai_bench_now_ns();
        kai_decref(r);
        kai_bench_record(i, b - a);
    }
    kai_bench_finalize(desc, iters);
}

/* ---------- check harness hooks (used by --check runs) ----------
 * check v1 (issue #44): property-based testing. Each `check "..."
 * [with p: T, ...] { body }` block is executed KAI_CHECK_ITERS
 * times. Each iteration generates random values for the with-clause
 * params via kai_arbitrary_<T>() and runs the body. The body must
 * produce a Bool — true means the property held for that input,
 * false records a counterexample and exits early.
 *
 * v1 reports the FIRST counterexample only (no shrinking). Shrinking
 * + median-based outlier-counterexample selection deferred to v1.x;
 * protocol-based generators (impl Arbitrary for T) deferred to v2
 * post-protocols maturity (see issue #44 split plan).
 */

#define KAI_CHECK_ITERS 100

static int kai_check_count_total = 0;
static int kai_check_count_passed = 0;
static const char *kai_check_current_desc = NULL;

#define KAI_CHECK_CX_BUF 1024
static char kai_check_cx_buf[KAI_CHECK_CX_BUF];
static size_t kai_check_cx_len = 0;

/* xorshift64* PRNG. Seeded once per process with a constant so
   counterexample reports are reproducible run-to-run; reseed via
   KAI_CHECK_SEED env var lands in v1.x. */
static uint64_t kai_check_seed = 0xC0DECAFE12345678ULL;

static uint64_t kai_check_rand_u64(void) {
    kai_check_seed ^= kai_check_seed >> 12;
    kai_check_seed ^= kai_check_seed << 25;
    kai_check_seed ^= kai_check_seed >> 27;
    return kai_check_seed * 0x2545F4914F6CDD1DULL;
}

static void kai_check_cx_reset(void) {
    kai_check_cx_buf[0] = '\0';
    kai_check_cx_len = 0;
}

static void kai_check_cx_append_raw(const char *s, size_t n) {
    if (!s || n == 0) return;
    if (kai_check_cx_len + n + 1 >= KAI_CHECK_CX_BUF) {
        n = KAI_CHECK_CX_BUF - 1 - kai_check_cx_len;
        if (n == 0) return;
    }
    memcpy(kai_check_cx_buf + kai_check_cx_len, s, n);
    kai_check_cx_len += n;
    kai_check_cx_buf[kai_check_cx_len] = '\0';
}

static void kai_check_cx_append(const char *s) {
    if (!s) return;
    kai_check_cx_append_raw(s, strlen(s));
}

/* Records "name=<repr>" in the counterexample buffer. Called by the
   emitted check fn after each kai_arbitrary_<T> generator inside the
   per-iter loop, before evaluating the body, so that on failure the
   buffer already holds every input the predicate saw. v repr uses
   kai_to_string (borrow). */
static void kai_check_record_param(const char *name, KaiValue *v) {
    if (kai_check_cx_len > 0) kai_check_cx_append(", ");
    kai_check_cx_append(name);
    kai_check_cx_append("=");
    KaiValue *s = kai_to_string(v);
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        kai_check_cx_append_raw(s->as.s.bytes, s->as.s.len);
    }
    kai_decref(s);
}

static void kai_check_begin(const char *desc) {
    kai_check_count_total++;
    kai_check_current_desc = desc;
}

static void kai_check_pass(int iters) {
    kai_check_count_passed++;
    fprintf(stderr, "  %s: %d iter, OK\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iters);
}

static void kai_check_fail(int iter_at) {
    fprintf(stderr, "  %s: counterexample at iter %d: %s\n",
            kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
            iter_at, kai_check_cx_buf);
}

static int kai_check_summary(void) {
    fprintf(stderr, "\n%d/%d checks passed\n",
            kai_check_count_passed, kai_check_count_total);
    return (kai_check_count_passed == kai_check_count_total) ? 0 : 1;
}

/* Intrinsic generators (issue #44 path b). Each returns a fresh
   KaiValue * with rc=1 ready for the body to consume or drop. The
   ranges are kept small so counterexamples are human-readable
   (Int in [-50, 50], String len in [0, 10], list len in [0, 7]). */

static KaiValue *kai_arbitrary_int(void) {
    int64_t r = (int64_t)(kai_check_rand_u64() % 101) - 50;
    return kai_int(r);
}

static KaiValue *kai_arbitrary_bool(void) {
    return kai_bool((int)(kai_check_rand_u64() & 1));
}

static KaiValue *kai_arbitrary_char(void) {
    /* printable ASCII ('!' .. '~') */
    uint32_t c = (uint32_t)(0x21 + (kai_check_rand_u64() % 0x5E));
    return kai_char(c);
}

static KaiValue *kai_arbitrary_string(void) {
    int len = (int)(kai_check_rand_u64() % 11);
    char buf[16];
    for (int i = 0; i < len; i++) {
        buf[i] = (char)(0x21 + (kai_check_rand_u64() % 0x5E));
    }
    return kai_str_from_bytes(buf, (size_t) len);
}

/* List generators by element kind. v1 supports primitive elements
   only. Structural sum/record/list-of-non-primitive lands in v1.x
   together with the typer-side derivation. */
#define KAI_DEFINE_ARBITRARY_LIST(NAME, ELEM)                          \
    static KaiValue *NAME(void) {                                      \
        int len = (int)(kai_check_rand_u64() % 8);                     \
        KaiValue *acc = kai_nil();                                     \
        for (int i = 0; i < len; i++) acc = kai_cons(ELEM(), acc);     \
        return acc;                                                    \
    }
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_int,    kai_arbitrary_int)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_bool,   kai_arbitrary_bool)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_char,   kai_arbitrary_char)
KAI_DEFINE_ARBITRARY_LIST(kai_arbitrary_list_string, kai_arbitrary_string)
#undef KAI_DEFINE_ARBITRARY_LIST

/* ---------- shrinkers (issue #438) ---------------------------------
 * Each kai_shrink_<T>(v) returns a NEW KaiValue* (rc=1) that is one
 * greedy step closer to the canonical minimum, or NULL when no smaller
 * candidate exists. Callers own both the input (untouched, still must
 * be decref'd by the caller as before) and the returned value.
 *
 * Strategy is greedy and per-type:
 *   Int    : halve toward 0 (sign-preserving).
 *   Bool   : true → false; false → NULL.
 *   Char   : bisect toward 'a'; if c == 'a' then NULL.
 *   String : delete first byte; else if non-empty, shrink first non-'a'
 *            byte toward 'a'; else NULL.
 *   List   : delete head element; else shrink first element via the
 *            per-T element shrinker; else NULL.
 *
 * Greedy means each step produces ONE candidate; the runner accepts it
 * only if the predicate still fails. If accepted the runner shrinks
 * again from the new candidate; if rejected the runner asks for the
 * NEXT alternative — encoded by re-calling the shrinker on the same
 * input, which is fine because each shrinker is deterministic and the
 * runner falls through to a different param after one rejection (the
 * only branch we care about for v1.x; integrated multi-strategy
 * backtracking is post-1.0 per #438 out-of-scope). */

static KaiValue *kai_shrink_int(KaiValue *v) {
    if (!kai_is_int(v)) return NULL;
    /* `v` may be a Koka-style tagged immediate (small Int) OR a heap
       KAI_INT, so read through kai_intf — a raw `v->as.i` deref would
       segv on the immediate (which has no header). */
    int64_t x = kai_intf(v);
    if (x == 0) return NULL;
    /* Halve toward zero. Integer division on negatives in C is
       truncation toward zero (C99 §6.5.5/6), so x/2 already does the
       right thing for both signs (e.g. -8/2 = -4, -1/2 = 0). */
    return kai_int(x / 2);
}

static KaiValue *kai_shrink_bool(KaiValue *v) {
    if (!v || v->tag != KAI_BOOL) return NULL;
    if (v->as.b == 0) return NULL;
    return kai_bool(0);
}

static KaiValue *kai_shrink_char(KaiValue *v) {
    if (!v || v->tag != KAI_CHAR) return NULL;
    uint32_t c = v->as.c;
    if (c == (uint32_t) 'a') return NULL;
    /* Bisect toward 'a'. Converges in O(log) steps. */
    uint32_t a = (uint32_t) 'a';
    uint32_t next = (c > a) ? (a + (c - a) / 2) : (c + (a - c) / 2);
    if (next == c) next = a;  /* defensive: ensure progress */
    return kai_char(next);
}

static KaiValue *kai_shrink_string(KaiValue *v) {
    if (!v || v->tag != KAI_STR) return NULL;
    size_t len = v->as.s.len;
    if (len > 0) {
        /* Delete first byte. */
        return kai_str_from_bytes(v->as.s.bytes + 1, len - 1);
    }
    return NULL;
}

/* List shrinker factory. Two strategies:
   A) drop head — biggest single reduction, collapses toward nil.
   B) elem shrink — walks the spine looking for the FIRST element
      that still has a smaller form via ELEM_SHRINK and rebuilds the
      list with that one element replaced. Returns NULL if every
      element is at its own minimum.
   The walk-and-replace pattern gives strategy B a deterministic
   linear sweep across the full list, not just the head, so e.g.
   [0, -30] still has a productive shrink (rebuild to [0, -15]). */
#define KAI_DEFINE_SHRINK_LIST(NAME, ELEM_SHRINK)                          \
    static KaiValue *NAME(KaiValue *v) {                                   \
        if (!v) return NULL;                                               \
        if (v->tag == KAI_NIL) return NULL;                                \
        if (v->tag != KAI_CONS) return NULL;                               \
        KaiValue *tail = v->as.cons.tail;                                  \
        kai_incref(tail);                                                  \
        return tail;                                                       \
    }                                                                      \
    /* Rebuild prefix (in reverse) onto a new tail. Helper for strategy B. \
       Each element in `rev_prefix` is a borrowed pointer and is incref'd  \
       as it lands in the new spine; the new spine takes ownership of the  \
       passed-in `tail` (no extra incref). */                              \
    static KaiValue *NAME##_rebuild(KaiValue **rev_prefix, int n,          \
                                    KaiValue *tail) {                      \
        KaiValue *acc = tail;                                              \
        for (int i = 0; i < n; i++) {                                      \
            KaiValue *h = rev_prefix[i];                                   \
            kai_incref(h);                                                 \
            acc = kai_cons(h, acc);                                        \
        }                                                                  \
        return acc;                                                        \
    }                                                                      \
    static KaiValue *NAME##_head(KaiValue *v) {                            \
        if (!v || v->tag != KAI_CONS) return NULL;                         \
        /* First pass: walk to find the first shrinkable element. */       \
        KaiValue *prefix[64];                                              \
        int prefix_n = 0;                                                  \
        KaiValue *cur = v;                                                 \
        while (cur != NULL && cur->tag == KAI_CONS) {                      \
            KaiValue *h = cur->as.cons.head;                               \
            KaiValue *h2 = ELEM_SHRINK(h);                                 \
            if (h2 != NULL) {                                              \
                /* Build new list: h2 :: cur->tail (incref'd) prepended    \
                   by the walked prefix in reverse. */                     \
                KaiValue *new_tail = cur->as.cons.tail;                    \
                kai_incref(new_tail);                                      \
                KaiValue *acc = kai_cons(h2, new_tail);                    \
                acc = NAME##_rebuild(prefix, prefix_n, acc);               \
                return acc;                                                \
            }                                                              \
            if (prefix_n >= 64) return NULL; /* bound spine walk */         \
            prefix[prefix_n++] = h;                                        \
            cur = cur->as.cons.tail;                                       \
        }                                                                  \
        return NULL;                                                       \
    }
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_int,    kai_shrink_int)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_bool,   kai_shrink_bool)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_char,   kai_shrink_char)
KAI_DEFINE_SHRINK_LIST(kai_shrink_list_string, kai_shrink_string)
#undef KAI_DEFINE_SHRINK_LIST

/* Per-process shrink-iteration cap. KAI_CHECK_SHRINK_ITERS in the env
   overrides; default 200 per #438. Read once and memoised so we don't
   pay getenv cost per shrink step. */
#define KAI_CHECK_SHRINK_ITERS_DEFAULT 200
static int kai_check_shrink_iters_cached = -1;
static int kai_check_shrink_iters_limit(void) {
    if (kai_check_shrink_iters_cached >= 0) return kai_check_shrink_iters_cached;
    const char *env = getenv("KAI_CHECK_SHRINK_ITERS");
    int n = KAI_CHECK_SHRINK_ITERS_DEFAULT;
    if (env && *env) {
        int parsed = atoi(env);
        if (parsed >= 0) n = parsed;
    }
    kai_check_shrink_iters_cached = n;
    return n;
}

/* Counterexample-buffer side channel for shrinking. When a failure
   triggers shrinking, the runner first snapshots the original cx
   buffer here so the final report can show "<orig> shrunk to <min>".
   Bounded by the same KAI_CHECK_CX_BUF cap as the live buffer. */
static char kai_check_orig_cx_buf[KAI_CHECK_CX_BUF];
static int  kai_check_has_orig_cx = 0;

static void kai_check_cx_save_orig(void) {
    size_t n = kai_check_cx_len;
    if (n >= KAI_CHECK_CX_BUF) n = KAI_CHECK_CX_BUF - 1;
    memcpy(kai_check_orig_cx_buf, kai_check_cx_buf, n);
    kai_check_orig_cx_buf[n] = '\0';
    kai_check_has_orig_cx = 1;
}

/* Reports a shrunk counterexample. If the shrink loop produced a
   strictly smaller value, prints both forms; otherwise (no progress)
   degrades to the v1 single-form output so noise stays low. */
static void kai_check_fail_shrunk(int iter_at) {
    if (kai_check_has_orig_cx
        && strcmp(kai_check_orig_cx_buf, kai_check_cx_buf) != 0) {
        fprintf(stderr,
                "  %s: counterexample at iter %d: %s, shrunk to %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_orig_cx_buf, kai_check_cx_buf);
    } else {
        fprintf(stderr, "  %s: counterexample at iter %d: %s\n",
                kai_check_current_desc ? kai_check_current_desc : "(unnamed)",
                iter_at, kai_check_cx_buf);
    }
    kai_check_has_orig_cx = 0;
}

/* Runtime-aware assert: called from every emitted `assert`. Inside an
   active test (kai_test_in_progress) it prints a failure and longjmps
   back to the test harness so the next test can run. Otherwise it
   aborts the process via kai_prelude_panic, matching stage 0's
   non-test-mode behaviour. */
static void kai_assert_check(KaiValue *cond, const char *msg) {
    int ok = kai_op_truthy(cond);
    kai_decref(cond);
    if (ok) return;
    if (kai_test_in_progress) {
        kai_test_fail(kai_test_current, msg ? msg : "assertion failed");
        longjmp(kai_test_jmp, 1);
    } else {
        kai_prelude_panic(kai_str(msg ? msg : "assertion failed"));
    }
}

/* Issue #86 (piece 2): contract-violation assert that appends the
   runtime value of the offending binding to the panic message. Used
   by the refinement-desugar path when the predicate has the simple
   shape `<ident> <binop> <literal>` and the emitter could statically
   resolve the single ident's local. `base_msg` is the predicate-aware
   piece-1 context ("requires violated in `<fn>`\nrequired: ..."); the
   appended line is "\nargument <ident_name> was: <value>".

   Ownership: `cond` and `val` are both CONSUMED (decref'd on every
   path). The call site passes a fresh owned ref for `val` — a boxed
   local is forwarded via `kai_incref`, a raw-unboxed scalar via a
   fresh `kai_real`/`kai_int` box. `kai_to_string` is a borrow on its
   argument, so we decref `val` ourselves once the string is built.
   Every owned temp (`vs`, `full`, `val`) is released before the
   longjmp so a failure inside an active test block does not leak
   (KAI_TRACE_RC / ASAN gate the lane on this). */
static void kai_assert_check_with_value(KaiValue *cond, const char *base_msg,
                                        const char *ident_name, KaiValue *val) {
    int ok = kai_op_truthy(cond);
    kai_decref(cond);
    if (ok) { kai_decref(val); return; }
    KaiValue *vs   = kai_to_string(val);
    kai_decref(val);
    KaiValue *m0   = kai_str(base_msg ? base_msg : "assertion failed");
    KaiValue *m1   = kai_string_concat(m0, kai_str("\nargument "));
    KaiValue *m2   = kai_string_concat(m1, kai_str(ident_name ? ident_name : "?"));
    KaiValue *m3   = kai_string_concat(m2, kai_str(" was: "));
    KaiValue *full = kai_string_concat(m3, vs);
    kai_decref(m0); kai_decref(m1); kai_decref(m2); kai_decref(m3); kai_decref(vs);
    if (kai_test_in_progress) {
        kai_test_fail(kai_test_current, full->as.s.bytes);
        kai_decref(full);
        longjmp(kai_test_jmp, 1);
    } else {
        kai_prelude_panic(full);
    }
}

/* =================================================================
 * Effects: handler-stack runtime (m7a #5)
 * =================================================================
 *
 * Per docs/effects-impl.md §*Handler-stack runtime*. Each fiber
 * owns a stack of Evidence nodes; m7a operates with a single
 * implicit fiber (kai_main_fiber), but the layout is per-fiber so
 * m8's real scheduler can introduce fibers without refactoring
 * this part of the runtime (Doc C OQ #3, decided).
 *
 * The Evidence node itself is intentionally untyped on the
 * `handler` slot: each effect's compiled `Ev<Eff>` struct lives
 * elsewhere; the call site casts back to *Ev<Eff> at the point
 * where the effect name is statically known.
 *
 * m7a #6 will wire `handle { ... } with Eff { ... }` lowering to
 * call kai_evidence_push / kai_evidence_pop, and `Eff.op(args)` to
 * call kai_evidence_lookup.
 */

/* m7a #6a: handler-id stamped onto each EvE struct at handle entry,
 * and copied into the continuation closure for one-shot diagnostics
 * (Doc C §*resume representation*). The id is cosmetic — the
 * status byte in the continuation is the real one-shot check.
 * Monotonic + unique-per-process is enough for v1; m8 may revisit
 * if cross-fiber id collisions become a debugging concern. */
typedef unsigned long long KaiHandlerId;

static KaiHandlerId kai_next_handler_id = 1;

static KaiHandlerId kai_fresh_handler_id(void) {
    return kai_next_handler_id++;
}

/* m7a #6d: continuation closure, stack-allocated at every op call
 * site (Doc C §*resume representation* one-shot path). `status` is
 * the one-shot check: the first call to `resume` flips it from
 * UNRESUMED to RESUMED, the second call aborts with a runtime
 * diagnostic. `fn` + `env` together name the rest of the
 * computation; m7a #6d ships only the identity continuation
 * (kai_cont_identity), so resume is effectively a tail return of
 * its argument. The full CPS reification (the rest of the caller's
 * body as a separately emitted function) is a later milestone. */
typedef enum {
    KAI_CONT_UNRESUMED = 0,
    KAI_CONT_RESUMED   = 1
} KaiContStatus;

typedef struct KaiCont KaiCont;
struct KaiCont {
    KaiContStatus  status;
    void          *env;
    KaiValue     *(*fn)(void *env, KaiValue *v);
    KaiHandlerId   handler_id;
};

/* Identity continuation: returns its argument unchanged. Until the
 * CPS transform reifies the rest of the caller's body, every op
 * call site uses this as the resume target — so the one-shot check
 * is observable but the continuation is functionally a no-op. */
static KaiValue *kai_cont_identity(void *env, KaiValue *v) {
    (void) env;
    return v;
}

static void kai_cont_init_identity(KaiCont *k, KaiHandlerId hid) {
    k->status     = KAI_CONT_UNRESUMED;
    k->env        = NULL;
    k->fn         = &kai_cont_identity;
    k->handler_id = hid;
}

/* Surface `resume(v)` lowers to this. The check + flip + tail call
 * all happen here; the clause body sees a single function call. */
static KaiValue *kai_cont_resume(KaiCont *k, KaiValue *v) {
    if (k->status != KAI_CONT_UNRESUMED) {
        fprintf(stderr,
            "kai: continuation resumed twice (handler #%llu)\n",
            (unsigned long long) k->handler_id);
        exit(1);
    }
    k->status = KAI_CONT_RESUMED;
    return k->fn(k->env, v);
}

/* m12.8 Phase 4b: split Console into atomic Stdout (print) and
 * Stderr (eprint) default handlers. Both write the string + a
 * trailing '\n' and resume with `()`. *Self is opaque to the
 * runtime because `EvStdout` / `EvStderr` are compiler-emitted
 * types — the cast happens at the assignment in the main wrapper.
 *
 * EPIPE absorption (Doc B §`Console`/Default handler) is later
 * polish — for now any write fault propagates through fputs's
 * default behaviour. The minimal-but-useful path lands first. */
static KaiValue *kai_default_stdout_print(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
    }
    fputc('\n', stdout);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_stderr_eprint(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
    }
    fputc('\n', stderr);
    return kai_cont_resume(k, kai_unit());
}

/* m7a #7: default Fail handler. Doc B §`Fail` declares the op as
 * returning Nothing, so the handler must short-circuit — no resume,
 * no return value. The runtime writes a banner to stderr and exits
 * with status 1. Long-term Doc B prefers an unhandled-Fail compile
 * error (catalog says "no (unhandled = compile error)"); until the
 * typer's effect-row check in #8 enforces that, this default is
 * the safe fallback. */
static KaiValue *kai_default_fail_fail(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    (void) k;
    fputs("kai: Fail.fail: ", stderr);
    if (kai_is_ptr(msg) && msg->tag == KAI_STR) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    exit(1);
}

/* m7a #7 + Issue #620 — Phase R3 reactor: default Stdin handler.
 * Doc B §`Stdin` declares `read_line() : Option[String] / Fail`;
 * m7a simplifies to `: Option[String]`. EOF maps to None; any byte
 * read returns Some(line) with the trailing '\n' stripped if
 * present.
 *
 * R3 wiring: fd 0 is flipped to O_NONBLOCK once per process, then
 * read() loops accumulating bytes. On EAGAIN the fiber parks on
 * `kai_reactor_stdin_waiter` and the scheduler's poll() loop wakes
 * it when POLLIN / POLLHUP arrives on STDIN_FILENO. Multiple
 * concurrent readers are a logic bug (the bytes shred between
 * fibers); the second reader panics with a clear diagnostic. */
static KaiValue *kai_default_stdin_read_line(void *self, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    kai_reactor_stdin_set_nonblocking();

    size_t cap = KAI_READ_BUF_INIT, n = 0;
    char *buf = (char *) malloc(cap);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }

    for (;;) {
        if (n + 1 >= cap) { cap *= 2; buf = (char *) realloc(buf, cap); }
        ssize_t r = read(STDIN_FILENO, buf + n, 1);
        if (r > 0) {
            if (buf[n] == '\n') {
                /* Strip the trailing newline. */
                KaiValue *s = kai_str_from_bytes(buf, n);
                free(buf);
                KaiValue *some = kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = s}});
                return kai_cont_resume(k, some);
            }
            n++;
        } else if (r == 0) {
            /* EOF — peer closed. Partial line returned as Some;
             * empty buffer becomes None. */
            if (n == 0) {
                free(buf);
                return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
            }
            KaiValue *s = kai_str_from_bytes(buf, n);
            free(buf);
            KaiValue *some = kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = s}});
            return kai_cont_resume(k, some);
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* No bytes available — park the fiber on the reactor
             * until POLLIN fires on stdin. */
            if (kai_reactor_park_stdin(kai_current_fiber()) != 0) {
                fprintf(stderr,
                    "kai: stdin: multiple fibers reading concurrently "
                    "is undefined; serialize via an actor\n");
                exit(1);
            }
            /* Resumed: drain helper cleared the waiter slot. Loop
             * to retry the read. */
        } else if (errno == EINTR) {
            /* Signal interrupted the read — retry immediately. */
            continue;
        } else {
            /* Real I/O error. The op's surface type is
             * `Option[String]`; propagate as None and let the
             * caller see end-of-stream. Keeps parity with the
             * pre-R3 fgetc path which silently treated errors as
             * EOF. */
            free(buf);
            return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
        }
    }
}

/* Issue #453 + #620 — Phase R3: default Stdin.read_bytes handler.
 * Returns a String of at most `n` raw bytes; on EOF the returned
 * String is shorter than `n` — possibly empty. No Result wrapper —
 * the LSP framing use case treats a short read as end-of-stream.
 *
 * R3 wiring: same shape as read_line — non-blocking read() loop
 * with reactor parking on EAGAIN. The buffer is sized once up
 * front so partial reads accumulate without realloc. */
static KaiValue *kai_default_stdin_read_bytes(void *self, KaiValue *n, KaiCont *k) {
    (void) self;
    int64_t want = 0;
    if (n && kai_is_int(n) && kai_intf(n) > 0) want = kai_intf(n);
    if (n) kai_decref(n);
    if (want <= 0) {
        return kai_cont_resume(k, kai_str_from_bytes("", 0));
    }

    kai_reactor_init();
    kai_reactor_stdin_set_nonblocking();

    char *buf = (char *) malloc((size_t) want);
    if (!buf) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    size_t got = 0;

    while (got < (size_t) want) {
        ssize_t r = read(STDIN_FILENO, buf + got, (size_t) want - got);
        if (r > 0) {
            got += (size_t) r;
        } else if (r == 0) {
            /* EOF — return what we have. */
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (kai_reactor_park_stdin(kai_current_fiber()) != 0) {
                fprintf(stderr,
                    "kai: stdin: multiple fibers reading concurrently "
                    "is undefined; serialize via an actor\n");
                exit(1);
            }
        } else if (errno == EINTR) {
            continue;
        } else {
            /* Real I/O error — surface as short read (matches
             * fread's silent behaviour on the pre-R3 path). */
            break;
        }
    }

    KaiValue *s = kai_str_from_bytes(buf, got);
    free(buf);
    return kai_cont_resume(k, s);
}

/* m7a #7: default Env handlers. `args()` reuses kai_prelude_args
 * (returns a [String] of argv[1..]); `var(name)` wraps getenv:
 * present → Some(value), absent → None. */
static KaiValue *kai_default_env_args(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_args());
}

static KaiValue *kai_default_env_get(void *self, KaiValue *name, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR) {
        return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
    }
    char nbuf[KAI_ENV_NAME_BUF];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    const char *got = getenv(nbuf);
    if (!got) return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
    KaiValue *s = kai_str(got);
    KaiValue *some = kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = s}});
    return kai_cont_resume(k, some);
}

/* Issue #127: Env write side + full block enumeration. POSIX
 * `setenv(name, value, 1)` copies its arguments into libc-owned
 * storage on both glibc and Apple libc, so the local stack buffers
 * here are safe to release on return — no buffer-ownership leak
 * across the FFI boundary. Errors lift `strerror(errno)` into a
 * fresh `Err(String)`. `vars()` walks the POSIX `environ` array,
 * splitting each `KEY=VALUE` entry into a `Pair { fst, snd }` so
 * the surface type lines up with `[(String, String)]`. */
extern char **environ;

static KaiValue *kai_default_env_set_var(void *self, KaiValue *name,
                                          KaiValue *value, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR || !value || value->tag != KAI_STR) {
        KaiValue *m = kai_str("set_var: name and value must be Strings");
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    char nbuf[KAI_ENV_NAME_BUF];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    /* setenv copies value too; an arbitrarily-long content string can
     * outgrow the stack buffer, so heap-dup once and free after the
     * call returns. */
    char *vbuf = (char *) malloc(value->as.s.len + 1);
    if (!vbuf) {
        KaiValue *m = kai_str("set_var: out of memory");
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    if (value->as.s.len > 0) memcpy(vbuf, value->as.s.bytes, value->as.s.len);
    vbuf[value->as.s.len] = '\0';
    int rc = setenv(nbuf, vbuf, 1);
    int saved_errno = errno;
    free(vbuf);
    if (rc != 0) {
        const char *msg = strerror(saved_errno);
        if (!msg) msg = "set_var failed";
        KaiValue *m = kai_str(msg);
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    KaiValue *u = kai_unit();
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_env_unset_var(void *self, KaiValue *name, KaiCont *k) {
    (void) self;
    if (!name || name->tag != KAI_STR) {
        KaiValue *m = kai_str("unset_var: name must be a String");
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    char nbuf[KAI_ENV_NAME_BUF];
    size_t nlen = name->as.s.len < sizeof(nbuf) - 1 ? name->as.s.len : sizeof(nbuf) - 1;
    memcpy(nbuf, name->as.s.bytes, nlen);
    nbuf[nlen] = '\0';
    if (unsetenv(nbuf) != 0) {
        const char *msg = strerror(errno);
        if (!msg) msg = "unset_var failed";
        KaiValue *m = kai_str(msg);
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    KaiValue *u = kai_unit();
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_env_vars(void *self, KaiCont *k) {
    (void) self;
    /* Build the list head-down via list_append so the surface order
     * matches the libc enumeration order (first entry of `environ`
     * appears as head). cons-then-reverse would also work; this
     * shape mirrors how `kai_prelude_args` walks argv. */
    KaiValue *acc = kai_nil();
    int count = 0;
    for (char **ep = environ; ep && *ep; ++ep) ++count;
    for (int i = count - 1; i >= 0; --i) {
        const char *entry = environ[i];
        if (!entry) continue;
        const char *eq = strchr(entry, '=');
        size_t name_len  = eq ? (size_t) (eq - entry) : strlen(entry);
        const char *vstart = eq ? eq + 1 : "";
        size_t value_len = eq ? strlen(vstart) : 0;
        KaiValue *name_kv  = kai_str_from_bytes(entry, name_len);
        KaiValue *value_kv = kai_str_from_bytes(vstart, value_len);
        KaiValue *fields[2] = { name_kv, value_kv };
        static const char *names[2] = { "fst", "snd" };
        KaiValue *pair = kai_record(2, fields, names);
        acc = kai_cons(pair, acc);
    }
    return kai_cont_resume(k, acc);
}

/* m7a #7: default File handlers. Each reuses the prelude helper —
 * which already produces `Result[T, String]` shapes per Doc B
 * §`File` error model — but routes the call through the Phase R1
 * reactor's file-pool worker (issue #611) so the blocking `fopen`
 * / `fread` / `fwrite` syscall runs off the scheduler thread.
 * Other fibers therefore make forward progress while one is mid
 * file op.
 *
 * Each `_arg` struct lives on the calling fiber's stack for the
 * lifetime of the work; the `_thunk` runs on a pool worker thread
 * and returns the KaiValue * the prelude produced. */
typedef struct {
    KaiValue *path;
} KaiFileReadFileArg;
static KaiValue *_kai_file_read_file_thunk(void *arg) {
    KaiFileReadFileArg *a = (KaiFileReadFileArg *) arg;
    return kai_prelude_read_file(a->path);
}
static KaiValue *kai_default_file_read_file(void *self, KaiValue *path, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileReadFileArg a = { path };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_read_file_thunk, &a);
    return kai_cont_resume(k, r);
}

typedef struct {
    KaiValue *path;
    KaiValue *contents;
} KaiFileWriteFileArg;
static KaiValue *_kai_file_write_file_thunk(void *arg) {
    KaiFileWriteFileArg *a = (KaiFileWriteFileArg *) arg;
    return kai_prelude_write_file(a->path, a->contents);
}
static KaiValue *kai_default_file_write_file(void *self, KaiValue *path,
                                              KaiValue *contents, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileWriteFileArg a = { path, contents };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_write_file_thunk, &a);
    return kai_cont_resume(k, r);
}

/* Issue #771 Phase 1: default handlers for the five chunked `File`
 * ops. Each offloads its blocking syscall to the R1 pool worker and
 * parks the fiber, identical to read_file/write_file above — regular
 * files are not epoll-pollable, so the pool is the readiness path. The
 * `_arg` struct lives on the fiber stack for the duration of the work;
 * the `_thunk` runs on a pool thread and returns the prelude's value. */
typedef struct { KaiValue *path; } KaiFileOpenArg;
static KaiValue *_kai_file_open_read_thunk(void *arg) {
    return kai_prelude_file_open_read(((KaiFileOpenArg *) arg)->path);
}
static KaiValue *kai_default_file_open_read(void *self, KaiValue *path, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileOpenArg a = { path };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_open_read_thunk, &a);
    return kai_cont_resume(k, r);
}

typedef struct { KaiValue *h; KaiValue *max; } KaiFileReadChunkArg;
static KaiValue *_kai_file_read_chunk_thunk(void *arg) {
    KaiFileReadChunkArg *a = (KaiFileReadChunkArg *) arg;
    return kai_prelude_file_read_chunk(a->h, a->max);
}
static KaiValue *kai_default_file_read_chunk(void *self, KaiValue *h, KaiValue *max, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileReadChunkArg a = { h, max };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_read_chunk_thunk, &a);
    return kai_cont_resume(k, r);
}

static KaiValue *_kai_file_open_write_thunk(void *arg) {
    return kai_prelude_file_open_write(((KaiFileOpenArg *) arg)->path);
}
static KaiValue *kai_default_file_open_write(void *self, KaiValue *path, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileOpenArg a = { path };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_open_write_thunk, &a);
    return kai_cont_resume(k, r);
}

typedef struct { KaiValue *h; KaiValue *data; } KaiFileWriteChunkArg;
static KaiValue *_kai_file_write_chunk_thunk(void *arg) {
    KaiFileWriteChunkArg *a = (KaiFileWriteChunkArg *) arg;
    return kai_prelude_file_write_chunk(a->h, a->data);
}
static KaiValue *kai_default_file_write_chunk(void *self, KaiValue *h, KaiValue *data, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileWriteChunkArg a = { h, data };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_write_chunk_thunk, &a);
    return kai_cont_resume(k, r);
}

typedef struct { KaiValue *h; } KaiFileCloseArg;
static KaiValue *_kai_file_close_thunk(void *arg) {
    return kai_prelude_file_close(((KaiFileCloseArg *) arg)->h);
}
static KaiValue *kai_default_file_close_file(void *self, KaiValue *h, KaiCont *k) {
    (void) self;
    kai_reactor_init();
    KaiFileCloseArg a = { h };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_close_thunk, &a);
    return kai_cont_resume(k, r);
}

/* m7b #2b: default Mutable handler — wraps the prelude `array_*`
 * helpers and resumes with the result. Doc B §`Mutable` *Default
 * handler* says the trivial wrapping default is installed for any
 * main row that mentions Mutable. The op signatures here mirror
 * the prelude's Array[T] return shape (see compiler.kai
 * builtin_mutable_decl for the divergence note from Doc B). */
static KaiValue *kai_default_mutable_array_make(void *self, KaiValue *n,
                                                 KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_make(n, init));
}

static KaiValue *kai_default_mutable_array_length(void *self, KaiValue *a, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_length(a));
}

static KaiValue *kai_default_mutable_array_get(void *self, KaiValue *a,
                                                KaiValue *i, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_get(a, i));
}

static KaiValue *kai_default_mutable_array_set(void *self, KaiValue *a,
                                                KaiValue *i, KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_set(a, i, v));
}

static KaiValue *kai_default_mutable_array_grow(void *self, KaiValue *a,
                                                 KaiValue *n, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_array_grow(a, n, init));
}

/* Issue #257: Ref[T] ops in the default Mutable handler. Each
 * trampolines to its prelude helper and resumes with the result,
 * mirroring the array_* clauses above. */
static KaiValue *kai_default_mutable_ref_make(void *self, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_make(init));
}

static KaiValue *kai_default_mutable_ref_get(void *self, KaiValue *r, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_get(r));
}

static KaiValue *kai_default_mutable_ref_set(void *self, KaiValue *r,
                                              KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_prelude_ref_set(r, v));
}

/* Default Random handler — PCG32 (M.E.O'Neill 2014). Per-process
 * shared state, seeded once on first use from time(NULL). Doc B
 * §`Random` specifies seeding from SecureRandom; that is deferred
 * until SecureRandom lands. Non-cryptographic by design — for
 * games, simulations, sampling, fixtures. */
static uint64_t _kai_pcg_state  = 0x853c49e6748fea9bULL;
static uint64_t _kai_pcg_inc    = 0xda3e39cb94b95bdbULL;
static int      _kai_pcg_seeded = 0;

static uint32_t _kai_pcg32_next(void) {
    uint64_t old = _kai_pcg_state;
    _kai_pcg_state = old * 6364136223846793005ULL + (_kai_pcg_inc | 1ULL);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << (((uint32_t)(-(int32_t)rot)) & 31u));
}

static void _kai_pcg32_seed(uint64_t initstate, uint64_t initseq) {
    _kai_pcg_state = 0u;
    _kai_pcg_inc   = (initseq << 1u) | 1u;
    (void) _kai_pcg32_next();
    _kai_pcg_state += initstate;
    (void) _kai_pcg32_next();
}

static void _kai_pcg32_ensure_seeded(void) {
    if (!_kai_pcg_seeded) {
        uint64_t t = (uint64_t) time(NULL);
        _kai_pcg32_seed(t ^ 0xa5a5a5a5a5a5a5a5ULL, t * 6364136223846793005ULL);
        _kai_pcg_seeded = 1;
    }
}

/* Doc B §`Random`: int_range(lo, hi) returns a value in [lo, hi).
 * lo >= hi is a panic — there is no meaningful value. The PRNG draw
 * uses the `% delta` shortcut; for delta well under 2^32 (e.g. 52
 * for a card deck) the modulo bias is negligible. Rejection sampling
 * is a future polish. */
static KaiValue *kai_default_random_int_range(void *self, KaiValue *lo, KaiValue *hi, KaiCont *k) {
    (void) self;
    int64_t lo_v = (kai_is_int(lo)) ? kai_intf(lo) : 0;
    int64_t hi_v = (kai_is_int(hi)) ? kai_intf(hi) : 0;
    if (lo_v >= hi_v) {
        fprintf(stderr, "kai: Random.int_range: lo (%lld) >= hi (%lld)\n",
                (long long) lo_v, (long long) hi_v);
        exit(1);
    }
    _kai_pcg32_ensure_seeded();
    uint64_t delta = (uint64_t) (hi_v - lo_v);
    uint64_t draw;
    if (delta <= 0xFFFFFFFFULL) {
        draw = (uint64_t) _kai_pcg32_next() % delta;
    } else {
        uint64_t hi32 = (uint64_t) _kai_pcg32_next();
        uint64_t lo32 = (uint64_t) _kai_pcg32_next();
        draw = ((hi32 << 32) | lo32) % delta;
    }
    return kai_cont_resume(k, kai_int(lo_v + (int64_t) draw));
}

/* =================================================================
 * Clock default handler
 * =================================================================
 *
 * Spec: docs/effects-stdlib.md §`Clock`. Three ops:
 *   wall_now()       -> WallTime { secs, nanos } via CLOCK_REALTIME
 *   monotonic_now()  -> Instant  { secs, nanos } via CLOCK_MONOTONIC
 *   sleep_ns(ns)     -> Unit     via the Phase R1 reactor timer wheel
 *
 * Issue #611 — Phase R1: sleep_ns parks the calling fiber on the
 * reactor's timer wheel (sorted by CLOCK_MONOTONIC deadline) and
 * yields. The scheduler's poll() loop blocks until the next
 * deadline fires (or another wake source arrives) and promotes
 * the sleeper back to READY. A previous EINTR-based busy loop
 * around nanosleep(2) blocked the OS thread; the new path leaves
 * the rest of the scheduler free to run other fibers while one is
 * asleep.
 *
 * Field names ("secs", "nanos") are load-bearing: kai_op_field reads
 * the slot by strcmp on the name pointer's contents, so the static
 * cstrings here must match the kaikai-side `WallTime` / `Instant`
 * declarations in stdlib/time.kai exactly. */
static KaiValue *_kai_clock_make_record(int64_t secs, int64_t nanos) {
    KaiValue *secs_kv  = kai_int(secs);
    KaiValue *nanos_kv = kai_int(nanos);
    KaiValue *fields[2] = { secs_kv, nanos_kv };
    static const char *names[2] = { "secs", "nanos" };
    return kai_record(2, fields, names);
}

static KaiValue *kai_default_clock_wall_now(void *self, KaiCont *k) {
    (void) self;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        fprintf(stderr, "kai: Clock.wall_now: clock_gettime failed: %s\n",
                strerror(errno));
        exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

static KaiValue *kai_default_clock_monotonic_now(void *self, KaiCont *k) {
    (void) self;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        fprintf(stderr, "kai: Clock.monotonic_now: clock_gettime failed: %s\n",
                strerror(errno));
        exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

static KaiValue *kai_default_clock_sleep_ns(void *self, KaiValue *ns, KaiCont *k) {
    (void) self;
    int64_t ns_v = (kai_is_int(ns)) ? kai_intf(ns) : 0;
    if (ns_v > 0) {
        kai_reactor_init();
        uint64_t deadline = kai_reactor_now_ns() + (uint64_t) ns_v;
        kai_reactor_park_timer(kai_current_fiber(), deadline);
        /* Resumed by the timer-wheel drain. Cancel delivered while
         * the fiber is parked still arrives at the next yield-point
         * hook after we resume. */
    }
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * NetTcp default handler (net-tcp-v1)
 * =================================================================
 *
 * Spec: docs/effects-stdlib.md §`NetTcp`. Six ops, all blocking in
 * v1. Maps directly to POSIX sockets via libc — same FFI-to-libc
 * pattern as the File / Random defaults above. Errors return
 * `Err(strerror(errno))`; success wraps the handle in a record
 * matching the surface types declared by the compiler:
 *
 *   Conn      = { fd: Int }
 *   Listener  = { fd: Int, port: Int }
 *
 * The `port` slot on Listener carries the kernel-assigned port back
 * to the caller (the `bind(port=0)` use case). It is populated by
 * the listen op via getsockname; ordinary `bind(port=N)` callers
 * see the same N echoed back.
 *
 * v1 keeps to AF_INET (IPv4). AF_INET6 fallback waits on a separate
 * lane — calling out IPv4-only as a v1 limitation in the spec note.
 */

static KaiValue *_kai_net_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    if (!msg) msg = "unknown error";
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

static KaiValue *_kai_net_err_msg(KaiCont *k, const char *msg) {
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

/* Build a Conn record `{ fd }`. Static field-name strings match
 * what the kaikai-side `type Conn = { fd: Int }` declaration
 * produces from emit_record_constructor — kai_op_field reads by
 * strcmp on the name pointer's contents, so any pointer to a
 * matching cstring works. */
static KaiValue *_kai_net_make_conn(int fd) {
    KaiValue *fd_kv = kai_int((int64_t) fd);
    KaiValue *fields[1] = { fd_kv };
    static const char *names[1] = { "fd" };
    return kai_record(1, fields, names);
}

static KaiValue *_kai_net_make_listener(int fd, int port) {
    KaiValue *fd_kv   = kai_int((int64_t) fd);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { fd_kv, port_kv };
    static const char *names[2] = { "fd", "port" };
    return kai_record(2, fields, names);
}

/* Pull the `fd` slot out of a Conn / Listener record. Returns -1 if
 * the value is the wrong shape (caller falls through to an error
 * return); v1 trusts the typer to keep this honest. */
static int _kai_net_record_fd(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "fd") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

/* connect(host, port) -> Result[Conn, String]. host is a hostname
 * or IPv4 dotted-quad; getaddrinfo handles both. Restricted to
 * AF_INET in v1.
 *
 * Issue #630 — Phase R2: non-blocking connect. The fd is flipped to
 * O_NONBLOCK before connect(), which returns either 0 (instant
 * success — common for loopback) or -1 with errno == EINPROGRESS.
 * On EINPROGRESS the fiber parks on write-readiness; when the
 * handshake completes (or fails) the kernel marks the fd writable.
 * We read SO_ERROR to distinguish success from failure since
 * connect() itself does not run a second time. EAGAIN/EWOULDBLOCK
 * is not a documented connect() outcome and is treated identically
 * to EINPROGRESS for safety. */
static KaiValue *kai_default_nettcp_connect(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "connect: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%lld", (long long) kai_intf(port));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_buf, port_buf, &hints, &res);
    if (gai != 0) {
        const char *msg = gai_strerror(gai);
        return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
    }
    kai_reactor_init();
    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { saved_errno = errno; continue; }
        kai_socket_set_nonblock(fd);
        int crc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (crc == 0) {
            saved_errno = 0;
            break;  /* instant success — loopback path */
        }
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Park until the kernel marks the fd writable. POLLHUP /
             * POLLERR also wake us — drain treats them as ready and
             * the SO_ERROR check below surfaces the real reason. */
            kai_reactor_park_socket_write(kai_current_fiber(), fd);
            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0) {
                saved_errno = errno;
                close(fd);
                fd = -1;
                continue;
            }
            if (so_err == 0) {
                saved_errno = 0;
                break;  /* handshake succeeded */
            }
            saved_errno = so_err;
            close(fd);
            fd = -1;
            continue;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return _kai_net_err(k, saved_errno ? saved_errno : ECONNREFUSED);
    }
    KaiValue *conn = _kai_net_make_conn(fd);
    KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = conn}});
    return kai_cont_resume(k, ok);
}

/* listen(host, port) -> Result[Listener, String]. host = "" or
 * "0.0.0.0" binds INADDR_ANY; specific IPv4 string also works.
 * port = 0 asks the kernel for an ephemeral port; we read it back
 * via getsockname so callers don't need a separate effect op. */
static KaiValue *kai_default_nettcp_listen(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "listen: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);

    /* SO_REUSEADDR mirrors the convention every server example sets
     * — without it a listener that crashed seconds ago can't rebind
     * because the kernel still holds the port in TIME_WAIT. The
     * fixture's `bind(0)` path ignores the assigned port across
     * runs anyway, but explicit servers benefit. */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) kai_intf(port));
    if (host_buf[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host_buf, &addr.sin_addr) != 1) {
        /* Fall back to getaddrinfo for hostnames. */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host_buf, NULL, &hints, &res);
        if (gai != 0) {
            close(fd);
            const char *msg = gai_strerror(gai);
            return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
        }
        struct sockaddr_in *sin = (struct sockaddr_in *) res->ai_addr;
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }
    if (listen(fd, KAI_LISTEN_BACKLOG) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }
    /* Issue #630 — Phase R2: the listener fd must be non-blocking so
     * the parking accept() path below sees EAGAIN/EWOULDBLOCK when
     * no connection is queued. Without this the accept() syscall
     * would block the OS thread and starve every other fiber. */
    kai_socket_set_nonblock(fd);

    /* Read back the assigned port (kernel may have picked one when
     * the caller passed 0). */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    int actual_port = (int) kai_intf(port);
    if (getsockname(fd, (struct sockaddr *) &bound, &blen) == 0) {
        actual_port = (int) ntohs(bound.sin_port);
    }

    KaiValue *l  = _kai_net_make_listener(fd, actual_port);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = l}});
    return kai_cont_resume(k, ok);
}

/* accept(l) -> Result[Conn, String].
 *
 * Issue #630 — Phase R2: non-blocking accept. The listener fd was
 * flipped to O_NONBLOCK in listen(); accept() now returns -1 +
 * EAGAIN/EWOULDBLOCK when no peer is queued. The fiber parks on
 * read-readiness of the listener fd and retries on wake. The
 * connection fd inherits non-blocking on Linux when SOCK_NONBLOCK
 * is passed to accept4(); for portability (macOS lacks accept4)
 * we set it explicitly via kai_socket_set_nonblock on the returned
 * fd so subsequent send/recv on the Conn also park rather than
 * blocking. */
static KaiValue *kai_default_nettcp_accept(void *self, KaiValue *l, KaiCont *k) {
    (void) self;
    int lfd = _kai_net_record_fd(l);
    if (lfd < 0) return _kai_net_err_msg(k, "accept: invalid listener");
    kai_reactor_init();
    /* Defensive: listener fd should already be non-blocking from
     * listen(), but a future Listener constructed via FFI would not
     * be. Idempotent. */
    kai_socket_set_nonblock(lfd);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *) &peer, &plen);
        if (cfd >= 0) {
            kai_socket_set_nonblock(cfd);
            KaiValue *conn = _kai_net_make_conn(cfd);
            KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = conn}});
            return kai_cont_resume(k, ok);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kai_reactor_park_socket_read(kai_current_fiber(), lfd);
            /* On resume, kai_sched_park runs the cancel-yield check
             * for us (issue #679 fix) — a sibling-triggered cancel
             * longjmps to cancel_pad before the retry below. */
            continue;  /* Resume → retry accept() */
        }
        if (errno == EINTR) continue;
        return _kai_net_err(k, errno);
    }
}

/* send(c, data) -> Result[Int, String]. data is a [Byte] = [Int]
 * cons list; each element is taken mod 256. v1 walks the list once
 * to assemble a contiguous buffer, then writes it.
 *
 * Issue #630 — Phase R2: non-blocking send with a partial-writes
 * loop. The conn fd is already O_NONBLOCK (set by accept or
 * connect). send() may return fewer bytes than requested when the
 * kernel buffer is partially full, or -1 with EAGAIN when it is
 * completely full. The loop parks on write-readiness on EAGAIN and
 * advances on partial writes. The returned count is the total
 * bytes written — equal to the input length on success. The
 * pre-R2 contract that "callers may have to loop" is honoured
 * internally so user code never sees a short write. */
static KaiValue *kai_default_nettcp_send(void *self, KaiValue *c, KaiValue *data, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "send: invalid conn");
    if (!data) return _kai_net_err_msg(k, "send: null data");

    /* Count the cons cells; a [Byte] of millions of bytes is out of
     * scope for v1 — chunked send via streaming is a follow-up. */
    size_t n = 0;
    for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *) malloc(n);
        if (!buf) return _kai_net_err_msg(k, "send: out of memory");
        size_t i = 0;
        for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
            KaiValue *h = p->as.cons.head;
            int64_t b = (kai_is_int(h)) ? kai_intf(h) : 0;
            buf[i++] = (unsigned char) (b & 0xff);
        }
    }

    kai_reactor_init();
    /* Defensive: send may be called on a Conn that was never funnelled
     * through the local accept/connect path (e.g. constructed via FFI
     * in a future user lane). Idempotent on already-nonblock fds. */
    kai_socket_set_nonblock(fd);

    size_t total = 0;
    int saved_errno = 0;
    while (total < n) {
        /* MSG_NOSIGNAL stops a SIGPIPE from killing the process when
         * the peer has closed its read side; we surface EPIPE through
         * the Result-shaped return instead. macOS does not have
         * MSG_NOSIGNAL but exposes SO_NOSIGPIPE on the socket; we
         * conservatively pass the flag where it exists and rely on
         * the default SIGPIPE handler being SIG_IGN-equivalent on
         * the install side for v1. */
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, buf + total, n - total, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, buf + total, n - total, 0);
#endif
        if (w > 0) {
            total += (size_t) w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            kai_reactor_park_socket_write(kai_current_fiber(), fd);
            continue;  /* Resume → retry send() */
        }
        if (w < 0 && errno == EINTR) continue;
        saved_errno = errno;
        break;
    }
    free(buf);
    if (saved_errno != 0 && total == 0) {
        return _kai_net_err(k, saved_errno);
    }

    KaiValue *cnt = kai_int((int64_t) total);
    KaiValue *ok  = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = cnt}});
    return kai_cont_resume(k, ok);
}

/* recv(c, max) -> Result[[Byte], String]. max = 0 panics per spec
 * (no useful "read zero bytes"); negative max is treated the same.
 * Peer clean-close (recv == 0) returns Ok([]) so callers can
 * distinguish from "not yet" via the empty list.
 *
 * Issue #630 — Phase R2: non-blocking recv. The fd is O_NONBLOCK
 * (set by accept or connect). recv() returns >0 (bytes available),
 * 0 (peer clean-close — surfaces as Ok([])), or -1 with EAGAIN
 * when the kernel buffer is empty. On EAGAIN the fiber parks on
 * read-readiness and retries. A single recv call returns whatever
 * the kernel has buffered; callers loop at the user level if they
 * need an exact byte count (matches POSIX recv semantics). */
static KaiValue *kai_default_nettcp_recv(void *self, KaiValue *c, KaiValue *max, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "recv: invalid conn");
    int64_t cap = (kai_is_int(max)) ? kai_intf(max) : 0;
    if (cap <= 0) {
        fputs("kai: NetTcp.recv: max must be > 0\n", stderr);
        exit(1);
    }
    if (cap > (1 << 20)) cap = 1 << 20;  /* 1 MiB ceiling for v1 */
    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv: out of memory");

    kai_reactor_init();
    /* Defensive idempotent flip — Conn fds from accept/connect are
     * already non-blocking, but a future FFI-constructed Conn would
     * not be. */
    kai_socket_set_nonblock(fd);

    ssize_t got;
    for (;;) {
        got = recv(fd, buf, (size_t) cap, 0);
        if (got >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kai_reactor_park_socket_read(kai_current_fiber(), fd);
            continue;  /* Resume → retry recv() */
        }
        if (errno == EINTR) continue;
        int saved_errno = errno;
        free(buf);
        return _kai_net_err(k, saved_errno);
    }
    KaiValue *acc = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = acc}});
    return kai_cont_resume(k, ok);
}

/* close(c) -> Unit. Spec says errors from close(2) are logged and
 * swallowed — the user can do nothing useful with the value, and
 * shutdown() is the right tool for "did everything flush?" anyway.
 * Returns kai_unit(). */
static KaiValue *kai_default_nettcp_close(void *self, KaiValue *c, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd >= 0) {
        if (close(fd) < 0) {
            /* Stderr only — the surface op returns Unit. */
            fprintf(stderr, "kai: NetTcp.close: %s\n", strerror(errno));
        }
    }
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * NetDns effect — issue #352. getaddrinfo(3) exposed as a standalone
 * capability so a component can resolve names without earning the
 * right to open sockets (NetTcp). The syscall shape is identical to
 * the one embedded in `kai_default_nettcp_connect`; the duplication
 * is small and intentional (sharing a helper is the out-of-scope
 * follow-up the issue names). AF_INET / SOCK_STREAM hints match the
 * TCP path so the address set resolve returns is the same set connect
 * would have walked — IPv4-only in v1, same limitation as NetTcp.
 * ================================================================= */

/* Build an IpAddr record `{ addr }` from an addrinfo node. The field
 * name "addr" is the runtime contract with the kaikai-side builtin
 * `type IpAddr = { addr: String }` (driver.kai builtin_ipaddr_decl);
 * kai_op_field reads it by strcmp, so the static cstring is enough. */
static KaiValue *_kai_net_make_ipaddr(struct addrinfo *p) {
    char ip_buf[INET_ADDRSTRLEN];
    struct sockaddr_in *sin = (struct sockaddr_in *) p->ai_addr;
    const char *txt = inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
    KaiValue *addr_kv = kai_str(txt ? txt : "");
    KaiValue *fields[1] = { addr_kv };
    static const char *names[1] = { "addr" };
    return kai_record(1, fields, names);
}

/* Build a `[IpAddr]` from the getaddrinfo result list, preserving the
 * order getaddrinfo returned (recursing tail-first keeps the cons
 * chain in source order without a reverse pass). Non-AF_INET nodes
 * are skipped — v1 is IPv4-only and the hints already filter, but
 * the guard keeps the inet_ntop above honest. */
static KaiValue *_kai_net_ipaddr_list(struct addrinfo *p) {
    if (!p) return kai_nil();
    KaiValue *tail = _kai_net_ipaddr_list(p->ai_next);
    if (p->ai_family != AF_INET || !p->ai_addr) return tail;
    return kai_cons(_kai_net_make_ipaddr(p), tail);
}

/* resolve(host) -> Result[String, [IpAddr]] (Err-first). host is a
 * hostname or IPv4 dotted-quad; getaddrinfo handles both. An empty
 * result list (no AF_INET addresses) still resolves to `Ok([])` —
 * the stdlib `resolve_first` turns that into its own Err so the
 * runtime stays a thin getaddrinfo shim. */
static KaiValue *kai_default_netdns_resolve(void *self, KaiValue *host, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR) {
        return _kai_net_err_msg(k, "resolve: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_buf, NULL, &hints, &res);
    if (gai != 0) {
        const char *msg = gai_strerror(gai);
        return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
    }
    KaiValue *list = _kai_net_ipaddr_list(res);
    freeaddrinfo(res);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = list}});
    return kai_cont_resume(k, ok);
}

/* =================================================================
 * NetUdp default handler (issue #354)
 * =================================================================
 *
 * Mirror of the NetUdp block in stage0/runtime.h. Stage 2 keeps its
 * own copy of the runtime (Koka-style Perceus RC), so the datagram
 * UDP handlers live here verbatim alongside the NetTcp family.
 *
 * Spec: docs/effects-stdlib.md §`NetUdp`. Four ops, all blocking in
 * v1 (no reactor parking yet — the m8.x reactor lifts NetTcp and
 * NetUdp together). socket(AF_INET, SOCK_DGRAM, 0) + bind / sendto /
 * recvfrom / close. Surface records:
 *
 *   UdpSocket  = { fd: Int, port: Int }
 *   SocketAddr = { host: String, port: Int }
 *
 * `[Byte]` lands as `[Int]`; SocketAddr.host is a textual IPv4
 * dotted-quad (inet_pton on send, inet_ntop on recv). IPv4 only.
 */

static KaiValue *_kai_net_make_udpsocket(int fd, int port) {
    KaiValue *fd_kv   = kai_int((int64_t) fd);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { fd_kv, port_kv };
    static const char *names[2] = { "fd", "port" };
    return kai_record(2, fields, names);
}

static KaiValue *_kai_net_make_sockaddr(const char *host, int port) {
    KaiValue *host_kv = kai_str(host);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { host_kv, port_kv };
    static const char *names[2] = { "host", "port" };
    return kai_record(2, fields, names);
}

static int _kai_net_sockaddr_host(KaiValue *v, char *out, size_t cap) {
    if (!v || v->tag != KAI_RECORD || cap == 0) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "host") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!f || f->tag != KAI_STR) return -1;
            size_t n = f->as.s.len < cap - 1 ? f->as.s.len : cap - 1;
            memcpy(out, f->as.s.bytes, n);
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

static int _kai_net_record_port(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "port") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

static KaiValue *kai_default_netudp_bind(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "bind: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int64_t port_i = kai_intf(port);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) port_i);
    if (host_buf[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host_buf, &addr.sin_addr) != 1) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host_buf, NULL, &hints, &res);
        if (gai != 0) {
            close(fd);
            const char *msg = gai_strerror(gai);
            return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
        }
        struct sockaddr_in *sin = (struct sockaddr_in *) res->ai_addr;
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    int actual_port = (int) port_i;
    if (getsockname(fd, (struct sockaddr *) &bound, &blen) == 0) {
        actual_port = (int) ntohs(bound.sin_port);
    }

    KaiValue *s  = _kai_net_make_udpsocket(fd, actual_port);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = s}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_send(void *self, KaiValue *sock, KaiValue *dst, KaiValue *data, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd < 0) return _kai_net_err_msg(k, "send: invalid socket");
    if (!data) return _kai_net_err_msg(k, "send: null data");

    char dst_host[KAI_NET_HOST_BUF];
    if (_kai_net_sockaddr_host(dst, dst_host, sizeof(dst_host)) != 0) {
        return _kai_net_err_msg(k, "send: invalid destination address");
    }
    int dst_port = _kai_net_record_port(dst);
    if (dst_port < 0) return _kai_net_err_msg(k, "send: invalid destination port");

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port   = htons((uint16_t) dst_port);
    if (inet_pton(AF_INET, dst_host, &dst_addr.sin_addr) != 1) {
        return _kai_net_err_msg(k, "send: destination host is not an IPv4 address");
    }

    size_t n = 0;
    for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *) malloc(n);
        if (!buf) return _kai_net_err_msg(k, "send: out of memory");
        size_t i = 0;
        for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
            KaiValue *h = p->as.cons.head;
            int64_t b = (kai_is_int(h)) ? kai_intf(h) : 0;
            buf[i++] = (unsigned char) (b & 0xff);
        }
    }

    ssize_t w;
    for (;;) {
#ifdef MSG_NOSIGNAL
        w = sendto(fd, buf, n, MSG_NOSIGNAL, (struct sockaddr *) &dst_addr, sizeof(dst_addr));
#else
        w = sendto(fd, buf, n, 0, (struct sockaddr *) &dst_addr, sizeof(dst_addr));
#endif
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    int saved_errno = (w < 0) ? errno : 0;
    free(buf);
    if (w < 0) return _kai_net_err(k, saved_errno);

    KaiValue *cnt = kai_int((int64_t) w);
    KaiValue *ok  = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = cnt}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_recv(void *self, KaiValue *sock, KaiValue *max, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd < 0) return _kai_net_err_msg(k, "recv: invalid socket");
    int64_t cap = (kai_is_int(max)) ? kai_intf(max) : 0;
    if (cap <= 0) {
        fputs("kai: NetUdp.recv: max must be > 0\n", stderr);
        exit(1);
    }
    if (cap > (1 << 16)) cap = 1 << 16;  /* IPv4 datagram ceiling */
    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv: out of memory");

    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t got;
    for (;;) {
        memset(&src, 0, sizeof(src));
        srclen = sizeof(src);
        got = recvfrom(fd, buf, (size_t) cap, 0, (struct sockaddr *) &src, &srclen);
        if (got >= 0) break;
        if (errno == EINTR) continue;
        int saved_errno = errno;
        free(buf);
        return _kai_net_err(k, saved_errno);
    }

    char src_host[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &src.sin_addr, src_host, sizeof(src_host))) {
        src_host[0] = '\0';
    }
    int src_port = (int) ntohs(src.sin_port);
    KaiValue *addr = _kai_net_make_sockaddr(src_host, src_port);

    KaiValue *bytes = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; bytes = kai_cons(kai_int((int64_t) buf[i]), bytes); }
    free(buf);

    KaiValue *pfields[2] = { addr, bytes };
    static const char *pnames[2] = { "fst", "snd" };
    KaiValue *pair = kai_record(2, pfields, pnames);
    KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = pair}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_close(void *self, KaiValue *sock, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd >= 0) {
        if (close(fd) < 0) {
            fprintf(stderr, "kai: NetUdp.close: %s\n", strerror(errno));
        }
    }
    return kai_cont_resume(k, kai_unit());
}


/* =================================================================
 * Signal effect — issue #107. POSIX SIGINT/SIGTERM/SIGHUP/SIGUSR1/
 * SIGUSR2 trap for graceful shutdown.
 *
 * Shape (v1, on/off/await):
 *   on(sig)  : Unit  -- block sig at the process level, mark subscribed
 *   off(sig) : Unit  -- unblock sig, drop from subscribed set
 *   await()  : Sig   -- sigwait on the subscribed set, return arrived
 *
 * Why sigwait and not an async sa_handler:
 *   The async path is restricted to async-signal-safe calls; building a
 *   KaiValue variant inside the handler is not. The handler would have
 *   to set a pending bit and let the next yield-point drain it — that
 *   matches the BEAM-faithful `on_cancel(sig)` shape sketched in the
 *   issue, which v1 cannot honour cleanly: `kai_default_cancel_raise`
 *   exits the program when main_fiber has no cancel_pad, so a runtime-
 *   triggered Cancel never runs the user's `with Cancel { raise(_) ->
 *   cleanup }`. The on/off/await shape sidesteps the async problem
 *   entirely: signals are blocked via sigprocmask, queued by the
 *   kernel, and synchronously dequeued by sigwait inside `await()`
 *   where the full runtime is available.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §Signal):
 *   - Posix only. Windows / WASM are out of scope.
 *   - `await()` blocks the OS thread. Other fibers cannot run while
 *     it is parked. Acceptable for the v1 use case (main parks on
 *     Signal after spawning workers).
 *   - SIGCHLD intentionally absent — Process.wait reaps children.
 *   - Real-time signals (SIGRTMIN+n) and siginfo_t are out of scope.
 */

typedef struct {
    int         signo;
    const char *name;
    int32_t     tag;       /* atom-style variant tag, docs/variant-tags.md */
} KaiSignalEntry;

static const KaiSignalEntry kai_signal_entries[] = {
    { SIGINT,  "SigInt",  4 },
    { SIGTERM, "SigTerm", 5 },
    { SIGHUP,  "SigHup",  6 },
    { SIGUSR1, "SigUsr1", 7 },
    { SIGUSR2, "SigUsr2", 8 },
    { 0,       NULL,      0 }
};

static int _kai_signal_from_variant(KaiValue *sig_v) {
    if (!sig_v || sig_v->tag != KAI_VARIANT) return 0;
    const char *n = kai_variant_name_of(sig_v->variant_tag);
    if (!n) return 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (strcmp(e->name, n) == 0) return e->signo;
    }
    return 0;
}

static KaiValue *_kai_signal_to_variant(int signo) {
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (e->signo == signo) {
            return kai_variant_u(e->tag, e->name, 0, 0, NULL);
        }
    }
    return NULL;
}

static sigset_t kai_signal_subscribed;
static int      kai_signal_subscribed_init = 0;

static void _kai_signal_init_subscribed(void) {
    if (!kai_signal_subscribed_init) {
        sigemptyset(&kai_signal_subscribed);
        kai_signal_subscribed_init = 1;
    }
}

/* Issue #671 — Phase R4: install an async-signal-safe sa_handler
 * for `signo` that writes the signo byte into the reactor's self-
 * pipe. Replaces v1's sigprocmask-only block, which relied on
 * sigwait() inside `signal_await` to dequeue. Under R4 the kernel
 * delivers the signal asynchronously to our handler (write(2) is
 * async-signal-safe), and the scheduler's poll() loop reads the
 * byte and wakes the parked fiber. */
static void _kai_signal_install_handler(int signo) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kai_reactor_signal_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART so a signal arriving during a syscall in another
     * thread does not abort that syscall — only the scheduler
     * thread's poll() needs to wake, and poll() returns -1/EINTR
     * cleanly on its own. The reactor already runs without
     * SA_RESTART on SIGCHLD because the wake byte is its own
     * notification; here SA_RESTART is fine because the wake byte
     * IS the signal payload. */
    sa.sa_flags = SA_RESTART;
    sigaction(signo, &sa, NULL);
}

static void _kai_signal_uninstall_handler(int signo) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(signo, &sa, NULL);
}

static KaiValue *kai_default_signal_on(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigaddset(&kai_signal_subscribed, signo);
        /* Make sure the signal is NOT blocked at the process level —
         * R4 wants the handler to fire, not for the kernel to queue
         * the signal until sigwait() pulls it. Idempotent. */
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, signo);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
        _kai_signal_install_handler(signo);
        /* Ensure the reactor self-pipe exists before any handler can
         * fire — kai_reactor_init is idempotent so paying the call
         * here costs nothing if the reactor is already up. */
        kai_reactor_init();
    }
    return kai_cont_resume(k, kai_unit());
}

/* off(sig): restore the default disposition. A signal that arrives
 * after `off` but before the SIG_DFL takes effect is best-effort —
 * sigaction is atomic w.r.t. the calling thread but not w.r.t.
 * concurrent signal delivery, the kernel may have already queued
 * one byte to the self-pipe that the next `signal_await` (if any)
 * will consume. Documented as a sharp edge in
 * docs/effects-stdlib.md §Signal. */
static KaiValue *kai_default_signal_off(void *self, KaiValue *sig_v, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    int signo = _kai_signal_from_variant(sig_v);
    if (signo > 0) {
        sigdelset(&kai_signal_subscribed, signo);
        _kai_signal_uninstall_handler(signo);
    }
    return kai_cont_resume(k, kai_unit());
}

/* await(): empty subscribed set adds SIGINT defensively so Ctrl-C
 * still wakes the caller. R4 path: park on the singleton signal
 * waiter slot, yield to the scheduler, and resume when the reactor
 * drains the self-pipe. The signo arrives in reactor_wait_status. */
static KaiValue *kai_default_signal_await(void *self, KaiCont *k) {
    (void) self;
    _kai_signal_init_subscribed();
    /* Defensive SIGINT subscription if nothing has been on()'d —
     * mirrors the v1 behaviour so existing code that does
     * `Signal.await()` directly without `Signal.on(SigInt)` still
     * traps Ctrl-C. */
    int any = 0;
    for (const KaiSignalEntry *e = kai_signal_entries; e->name; ++e) {
        if (sigismember(&kai_signal_subscribed, e->signo)) { any = 1; break; }
    }
    if (!any) {
        sigaddset(&kai_signal_subscribed, SIGINT);
        sigset_t one;
        sigemptyset(&one);
        sigaddset(&one, SIGINT);
        sigprocmask(SIG_UNBLOCK, &one, NULL);
        _kai_signal_install_handler(SIGINT);
        kai_reactor_init();
    }
    for (;;) {
        KaiFiber *me = kai_current_fiber();
        if (kai_reactor_park_signal(me) != 0) {
            /* v1 contract: only one fiber may sit in Signal.await()
             * at a time. The byte in the self-pipe carries no
             * identity, so two concurrent waiters would race over
             * who picks it up. Mirrors the R3 stdin-multiplex
             * panic. */
            kai_prelude_panic(kai_str(
                "signal_await: a fiber is already awaiting a signal — "
                "concurrent Signal.await() is undefined; "
                "serialize via an actor or supervisor"));
        }
        /* Drain set f->reactor_wait_status before unparking. A
         * spurious wake (signo == 0) loops back and re-parks. */
        int signo = me->reactor_wait_status;
        me->reactor_wait_status = 0;
        if (signo > 0) {
            KaiValue *v = _kai_signal_to_variant(signo);
            if (v) return kai_cont_resume(k, v);
        }
    }
}

/* =================================================================
 * Process effect — issue #126. POSIX subprocess primitives.
 *
 * Shape (v1):
 *   start(cmd, args)  : Child                       -- fork + execvp
 *   wait(c)           : Result[Exit, String]        -- waitpid blocking
 *   kill(c, sig: Int) : Result[Unit, String]        -- kill(2)
 *   exit(code)        : Nothing                     -- _exit(2)
 *
 * `Child = { pid: Int }` and `Exit = Exited(Int) | Signaled(Int)`
 * are declared by the compiler's `builtin_child_decl` /
 * `builtin_exit_decl` (stage2/compiler.kai) and minted here by
 * name, same convention as Conn / Listener for NetTcp.
 *
 * Divergences from docs/effects-stdlib.md §Process pinned in the
 * compiler's `builtin_process_decl` comment:
 *   1. start returns Child, not Result[Child, String]. fork/exec
 *      failure surfaces through `kai_panic`, matching the
 *      "primitive failure" convention used elsewhere in the runtime.
 *   2. kill takes a raw signo Int rather than the issue #107 Sig
 *      sum type — kill needs the full POSIX signal set including
 *      SIGKILL, which Sig deliberately excludes.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §Process
 * *What's not in v1*):
 *   - POSIX only. Windows / WASM out of scope.
 *   - All ops are blocking: `Process.wait` parks the OS thread
 *     inside `waitpid` rather than the calling fiber. The m8.x
 *     cooperative scheduler (landed v0.4.0) provides the suspend /
 *     resume primitives, but reactor-driven cancellation-aware
 *     waiting (`wait_or_kill`) still needs a SIGCHLD-aware reactor
 *     plug that registers the pid and wakes the fiber when the
 *     child terminates. Tier 2 follow-up tracked in
 *     docs/fibers-honesty-targets.md §Reactor.
 *   - No stdio pipe plumbing. `pipe_stdout` / `pipe_stdin` and the
 *     surface helpers (wait_or_kill, signal) live in the follow-up
 *     `stdlib/os/process.kai` lane on top of these four ops.
 *   - SIGCHLD handling is implicit through blocking waitpid; the
 *     Signal effect intentionally omits SIGCHLD per its catalog
 *     comment so the two effects don't fight over the disposition.
 */

/* Build a Child record `{ pid: Int }`. The "pid" field name is
 * load-bearing — `_kai_process_record_pid` reads the slot by
 * strcmp on the name pointer's contents, in lockstep with
 * `builtin_child_decl` in stage2/compiler.kai. */
static KaiValue *_kai_process_make_child(int pid) {
    KaiValue *pid_kv = kai_int((int64_t) pid);
    KaiValue *fields[1] = { pid_kv };
    static const char *names[1] = { "pid" };
    return kai_record(1, fields, names);
}

/* Pull the `pid` slot out of a Child record. Returns -1 on shape
 * mismatch — the caller surfaces that as an error path; v1 trusts
 * the typer to keep this honest in normal flows. */
static int _kai_process_record_pid(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "pid") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

/* Mint Exited(code) / Signaled(signo) — variant *names* are the
 * runtime contract with `builtin_exit_decl`. The typer-declared
 * shape is `Exited(Int)` / `Signaled(Int)`. The emitter's slot
 * representation for a variant Int field has flipped twice; this
 * runtime constructor must track whichever convention the emitter
 * reads, because the cell it mints is matched by emitter-generated
 * code:
 *   - #440 Phase 2: raw scalar in `.i64`, slot kind KAI_VAR_SLOT_INT.
 *   - #741: re-boxed to a tagged pointer (`{.ptr = kai_int(n)}`,
 *     slot_mask 0) when `variant_slot_kind` reported Int as a pointer
 *     kind and the emitter bound every slot via `.ptr`.
 *   - i64-inline Lane B (commit 1f4f66f): back to raw int64 in `.i64`
 *     with slot kind KAI_VAR_SLOT_INT. `variant_slot_kind(Int) == 1`,
 *     so the emitter constructs `Exited` as
 *     `kai_variant_u_fast(9, 1, {{.i64 = code}})` (mask 1) and the
 *     match reads `kai_var_slots(_scr)[0].i64` raw. With the old boxed
 *     mask-0 cell the match read the pointer bits as the exit code, so
 *     every `Exited(0)` looked non-zero (process_basic #741 went red).
 * Mirror the emitter exactly: raw `.i64`, mask KAI_VAR_SLOT_INT. The
 * mask also keeps the generic drop walker from decref-ing a raw int
 * as if it were a pointer. */
static KaiValue *_kai_process_make_exit_exited(int code) {
    KaiVarSlot s; s.i64 = (int64_t) code;
    return kai_variant_u(9, "Exited", 1, KAI_VAR_SLOT_INT, &s);
}

static KaiValue *_kai_process_make_exit_signaled(int signo) {
    KaiVarSlot s; s.i64 = (int64_t) signo;
    return kai_variant_u(10, "Signaled", 1, KAI_VAR_SLOT_INT, &s);
}

static KaiValue *_kai_process_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    if (!msg) msg = "unknown error";
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

/* Walk a [String] cons list once to count entries, then again to
 * copy the C strings into a NULL-terminated argv vector. The
 * vector and its element copies are owned by the caller and freed
 * after execvp returns / does not return. argv[0] is `cmd`; the
 * supplied `args` list fills argv[1..n]. */
static char **_kai_process_build_argv(const char *cmd, KaiValue *args, int *out_n) {
    int n = 1;  /* slot 0 is cmd */
    for (KaiValue *p = args; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    char **argv = (char **) malloc(((size_t) n + 1) * sizeof(char *));
    if (!argv) return NULL;
    argv[0] = strdup(cmd ? cmd : "");
    int i = 1;
    for (KaiValue *p = args; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
        KaiValue *h = p->as.cons.head;
        if (kai_is_ptr(h) && h->tag == KAI_STR) {
            argv[i] = strdup(h->as.s.bytes ? h->as.s.bytes : "");
        } else {
            argv[i] = strdup("");
        }
        ++i;
    }
    argv[n] = NULL;
    *out_n = n;
    return argv;
}

static void _kai_process_free_argv(char **argv, int n) {
    if (!argv) return;
    for (int i = 0; i < n; ++i) free(argv[i]);
    free(argv);
}

/* start(cmd, args) -> Child. fork + execvp; on failure of either
 * primitive, panic with strerror — start has no Result wrapper in
 * v1 (see the divergence note in builtin_process_decl).
 *
 * Issue #611 — install the SIGCHLD handler before fork() so that
 * a fast-exiting child cannot deliver SIGCHLD to the kernel's
 * default disposition (zombie reaper only) before the reactor is
 * armed. Idempotent. */
static KaiValue *kai_default_process_start(void *self, KaiValue *cmd, KaiValue *args, KaiCont *k) {
    (void) self;
    if (!cmd || cmd->tag != KAI_STR) {
        fputs("kai: Process.start: cmd must be a String\n", stderr);
        exit(1);
    }
    kai_reactor_init();
    /* cmd is heap-allocated by kai_str_from_bytes with a trailing
     * NUL, so passing bytes directly to execvp is safe. */
    const char *cmd_cstr = cmd->as.s.bytes ? cmd->as.s.bytes : "";
    int argc = 0;
    char **argv = _kai_process_build_argv(cmd_cstr, args, &argc);
    if (!argv) {
        fputs("kai: Process.start: out of memory building argv\n", stderr);
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        int e = errno;
        _kai_process_free_argv(argv, argc);
        fprintf(stderr, "kai: Process.start: fork: %s\n", strerror(e));
        exit(1);
    }
    if (pid == 0) {
        /* Child: replace image. execvp searches PATH for relative
         * names; absolute paths bypass the search. On any failure
         * exit 127, the shell convention for "command not found". */
        execvp(cmd_cstr, argv);
        /* execvp only returns on failure. Async-signal-safe writers
         * only — keep the message minimal. */
        const char *prefix = "kai: Process.start: execvp: ";
        const char *msg    = strerror(errno);
        ssize_t w;
        w = write(2, prefix, strlen(prefix)); (void) w;
        if (msg) { w = write(2, msg, strlen(msg)); (void) w; }
        w = write(2, "\n", 1); (void) w;
        _exit(127);
    }
    /* Parent: clean up argv copies and resume with the Child
     * handle. The Child borrows nothing from argv — it carries
     * just the pid. */
    _kai_process_free_argv(argv, argc);
    KaiValue *child = _kai_process_make_child((int) pid);
    return kai_cont_resume(k, child);
}

/* wait(c) -> Result[Exit, String]. Issue #611 Phase R1: the wait
 * parks the calling fiber on the reactor's pid waiter map and the
 * SIGCHLD self-pipe drives the wake. WIFEXITED → Exited(status);
 * WIFSIGNALED → Signaled(signo). The reactor's waitpid drain uses
 * `-1` to harvest any pending child, so a SIGCHLD that fires while
 * the parking fiber is mid-park is handled by the drain helper at
 * the next reactor wait (or immediately if the byte was already
 * pending in the self-pipe). */
static KaiValue *kai_default_process_wait(void *self, KaiValue *child, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        KaiValue *m = kai_str("wait: invalid Child");
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    kai_reactor_init();
    /* Race: the child may have terminated before we registered the
     * waiter. Try a non-blocking waitpid first; if it succeeds we
     * report the status without parking. */
    int status = 0;
    pid_t rc = waitpid((pid_t) pid, &status, WNOHANG);
    if (rc == 0) {
        /* Child still running; park on the pid map and let the
         * SIGCHLD drain wake us with the status. */
        KaiFiber *me = kai_current_fiber();
        kai_reactor_park_pid(me, pid);
        status = me->reactor_wait_status;
        me->reactor_wait_pid    = 0;
        me->reactor_wait_status = 0;
    } else if (rc < 0) {
        return _kai_process_err(k, errno);
    }
    KaiValue *exit_v;
    if (WIFEXITED(status)) {
        exit_v = _kai_process_make_exit_exited(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        exit_v = _kai_process_make_exit_signaled(WTERMSIG(status));
    } else {
        exit_v = _kai_process_make_exit_exited(-1);
    }
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = exit_v}});
    return kai_cont_resume(k, ok);
}

/* kill(c, sig) -> Result[Unit, String]. Maps directly to kill(2);
 * sig is taken as a raw signo Int (full POSIX set including
 * SIGKILL — see the builtin_process_decl divergence note). */
static KaiValue *kai_default_process_kill(void *self, KaiValue *child, KaiValue *sig, KaiCont *k) {
    (void) self;
    int pid = _kai_process_record_pid(child);
    if (pid <= 0) {
        KaiValue *m = kai_str("kill: invalid Child");
        KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
        return kai_cont_resume(k, err);
    }
    int signo = (kai_is_int(sig)) ? (int) kai_intf(sig) : 0;
    if (kill((pid_t) pid, signo) < 0) {
        return _kai_process_err(k, errno);
    }
    KaiValue *u  = kai_unit();
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
    return kai_cont_resume(k, ok);
}

/* exit(code) -> Nothing. _exit(2) — skip libc atexit / stdio flush
 * to match the doc spec contract. The `: Nothing` return type is
 * load-bearing: we never resume k. */
static KaiValue *kai_default_process_exit(void *self, KaiValue *code, KaiCont *k) {
    (void) self;
    (void) k;
    int c = (kai_is_int(code)) ? (int) kai_intf(code) : 0;
    _exit(c);
    /* unreachable */
    return NULL;
}

/* =================================================================
 * Log effect — issue #141. Tier S2 #7 of `docs/stdlib-roadmap.md`.
 *
 * Four leveled ops (debug / info / warn / error) routed through the
 * Log effect. The default handler installed by `kai_main_install_
 * defaults` writes to stderr in
 *
 *     [YYYY-MM-DDTHH:MM:SSZ] LEVEL message\n
 *
 * form. The level field is left-padded to 5 chars so the column
 * after it is aligned across all four ops:
 *     "DEBUG", "INFO ", "WARN ", "ERROR".
 *
 * Timestamp source: `clock_gettime(CLOCK_REALTIME)` + `gmtime_r`
 * + `strftime`. Calling `clock_gettime` directly here (rather than
 * routing through the kaikai-side `Clock` effect) keeps the `Log`
 * row free of `Clock` — a program can declare `: Unit / Log` and
 * have its main install the Log default without also pulling
 * `Clock`'s default handler into the row.
 *
 * The runtime flushes stderr after each write so test harnesses
 * see the output deterministically (no buffer hold-back if a
 * later abort kills the process before exit-time flushing fires).
 *
 * v1 limitations (mirrored in stdlib/log.kai):
 *   - No level filtering. All four levels write unconditionally.
 *   - No structured fields, redaction, rotation, color, async
 *     batching, or trace-context propagation. All of those are
 *     ahu.log territory (a higher layer that wraps Log). */

static void _kai_log_emit(const char *level_padded, KaiValue *msg) {
    /* Format the timestamp into a stack buffer. clock_gettime /
     * gmtime_r failure is rare; fall back to a sentinel rather
     * than swallow the message — the user should still see what
     * level + body was intended. */
    char ts[32];
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        struct tm tm;
        if (gmtime_r(&now.tv_sec, &tm) != NULL &&
            strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm) > 0) {
            /* ok */
        } else {
            memcpy(ts, "?????", 6);
        }
    } else {
        memcpy(ts, "?????", 6);
    }

    /* Header `[<ts>] <LEVEL> ` then the message bytes then newline.
     * fputs on the prefix, fwrite on the message body so embedded
     * NULs in user strings don't truncate the line. */
    fputc('[', stderr);
    fputs(ts, stderr);
    fputs("] ", stderr);
    fputs(level_padded, stderr);
    fputc(' ', stderr);
    if (msg && msg->tag == KAI_STR && msg->as.s.bytes && msg->as.s.len > 0) {
        fwrite(msg->as.s.bytes, 1, msg->as.s.len, stderr);
    }
    fputc('\n', stderr);
    fflush(stderr);
}

static KaiValue *kai_default_log_debug(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("DEBUG", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_info(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("INFO ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_warn(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("WARN ", msg);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_log_error(void *self, KaiValue *msg, KaiCont *k) {
    (void) self;
    _kai_log_emit("ERROR", msg);
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * SecureRandom effect — issue #140. Cryptographically-secure RNG
 * deliberately separated from Random so test handlers stubbing the
 * latter cannot weaken security-sensitive paths.
 *
 * Default handler uses the platform CSPRNG, never a userspace PRNG:
 *   - Linux: getrandom(2) syscall (works pre-prng-init; we loop on
 *     EINTR but otherwise treat any failure as fatal — the kernel
 *     pool being unavailable is catastrophic and there is no
 *     meaningful Result for callers who already opted into crypto-
 *     grade randomness).
 *   - macOS / *BSD: arc4random_buf(3) — returns void; the libc
 *     implementation reseeds itself from /dev/urandom and panics
 *     internally on failure, matching the same "infallible from
 *     callers' perspective" contract.
 *
 * v1 limitations (mirrored in docs/effects-stdlib.md §SecureRandom):
 *   - POSIX only. Windows BCryptGenRandom is post-MVP.
 *   - No /dev/urandom fallback. getrandom is the entire Linux story.
 *   - No userspace ChaCha20 CSPRNG layer. Each draw hits the kernel.
 *   - Uniform `int(min, max)` uses `% delta` reduction. The modulo
 *     bias is acceptable for v1 (security-grade rejection sampling
 *     is a follow-up); cryptographic-protocol callers that need a
 *     strictly-uniform integer should draw `bytes` and reduce
 *     themselves.
 */

#if defined(__linux__)
#  include <sys/random.h>
#elif defined(__APPLE__) || defined(__MACH__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
   /* arc4random_buf is in <stdlib.h> on macOS/BSD, already included
    * above. No extra header needed. */
#else
#  error "kai SecureRandom: platform unsupported (POSIX getrandom / arc4random_buf required)"
#endif

static void _kai_securerandom_fill(unsigned char *buf, size_t n) {
#if defined(__linux__)
    size_t off = 0;
    while (off < n) {
        ssize_t got = getrandom(buf + off, n - off, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "kai: SecureRandom: getrandom failed: %s\n", strerror(errno));
            exit(1);
        }
        off += (size_t) got;
    }
#else
    arc4random_buf(buf, n);
#endif
}

/* int_range(min, max) -> Int in [min, max] (inclusive). Pulls 8
 * bytes from the platform CSPRNG, interprets them as a uint64,
 * reduces modulo (max - min + 1), and adds min. min > max is a
 * panic: there is no meaningful uniform draw over an empty range.
 * Op named `int_range` (not the doc's `int()`) because `int` is a
 * C keyword and the C backend emits each effect op as a struct
 * field by name. */
static KaiValue *kai_default_securerandom_int_range(void *self, KaiValue *min_v, KaiValue *max_v, KaiCont *k) {
    (void) self;
    int64_t lo = (kai_is_int(min_v)) ? kai_intf(min_v) : 0;
    int64_t hi = (kai_is_int(max_v)) ? kai_intf(max_v) : 0;
    if (lo > hi) {
        fprintf(stderr, "kai: SecureRandom.int: min (%lld) > max (%lld)\n",
                (long long) lo, (long long) hi);
        exit(1);
    }
    unsigned char buf[8];
    _kai_securerandom_fill(buf, sizeof(buf));
    uint64_t draw = 0;
    for (int i = 0; i < 8; ++i) draw = (draw << 8) | (uint64_t) buf[i];
    uint64_t span = (uint64_t) (hi - lo) + 1ULL;
    uint64_t pick = (span == 0) ? draw : (draw % span);
    return kai_cont_resume(k, kai_int(lo + (int64_t) pick));
}

/* bytes(n) -> [Int] (each in [0, 256), surface-typed [Byte] but
 * stage 2 has no first-class Byte yet — same divergence the NetTcp
 * decl documents). n <= 0 returns the empty list; n > 1 MiB is
 * capped (matches NetTcp.recv's v1 ceiling — multi-megabyte single
 * draws are out of scope and almost always wrong for crypto use,
 * which is happier with smaller, repeated draws). */
static KaiValue *kai_default_securerandom_bytes(void *self, KaiValue *n_v, KaiCont *k) {
    (void) self;
    int64_t n = (kai_is_int(n_v)) ? kai_intf(n_v) : 0;
    if (n <= 0) {
        return kai_cont_resume(k, kai_nil());
    }
    if (n > (1 << 20)) n = 1 << 20;
    unsigned char *buf = (unsigned char *) malloc((size_t) n);
    if (!buf) {
        fputs("kai: SecureRandom.bytes: out of memory\n", stderr);
        exit(1);
    }
    _kai_securerandom_fill(buf, (size_t) n);
    KaiValue *acc = kai_nil();
    for (int64_t i = n; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    return kai_cont_resume(k, acc);
}

/* =================================================================
 * m8.x cooperative scheduler primitives
 * =================================================================
 *
 * Spec: docs/fibers-impl.md §*Scheduler* and §*Yield primitives*.
 *
 * Single-threaded cooperative scheduler. The OS thread starts in
 * kai_main_fiber (state=RUNNING); spawned fibers each get a private
 * heap-allocated stack and a ucontext_t. Yield/park use swapcontext
 * to hand control between fibers. The dispatcher is implicit — there
 * is no separate scheduler context, just whoever was running before
 * the current fiber gets resumed when the queue empties.
 *
 * v1 ships Phase 2 (scheduler core); Phase 3 adds Cancel delivery at
 * yield points; Phase 4 adds blocking primitives (BlockSender mailbox
 * + Actor.receive parking); Phase 5 adds Link/Monitor runtime.
 */

/* Page size, queried once via sysconf and cached. macOS arm64 pages
 * are 16 KiB, x86_64 / Linux are 4 KiB; the guard arithmetic must use
 * the runtime value or mprotect rejects the call. */
static size_t kai_page_size(void) {
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t) ps : 4096;
    }
    return cached;
}

/* Read KAI_FIBER_STACK_SIZE once and cache. Out-of-range values
 * fall back to the default and log a warning. The result is rounded
 * up to a page-size multiple — mmap + mprotect both require it, and
 * 16 KiB pages on macOS arm64 silently break sub-page values
 * otherwise. */
static size_t kai_fiber_stack_size(void) {
    static size_t cached = 0;
    if (cached != 0) return cached;
    const char *env = getenv("KAI_FIBER_STACK_SIZE");
    size_t sz = 64 * 1024;  /* default 64 KiB */
    if (env && *env) {
        char *end = NULL;
        long long parsed = strtoll(env, &end, 10);
        if (end != env && *end == '\0' && parsed >= 4096 && parsed <= (long long)(64 * 1024 * 1024)) {
            sz = (size_t) parsed;
        } else {
            fprintf(stderr,
                "kai: KAI_FIBER_STACK_SIZE=%s out of range [4096, 64M]; using default 64K\n",
                env);
        }
    }
    size_t ps = kai_page_size();
    if (sz % ps != 0) sz = ((sz / ps) + 1) * ps;
    cached = sz;
    return cached;
}

/* SIGSEGV / SIGBUS handler for fiber stack overflow. The fault lands
 * on the active fiber's guard page (PROT_NONE); we print a diagnostic
 * and re-raise with the default disposition. Faults outside any guard
 * (e.g. NULL deref in user code) fall through to default — we only
 * decorate the stack-overflow case. The handler runs on a sigaltstack
 * so the overflowed stack is never used to format the message. Spec:
 * `docs/fibers-honesty-targets.md` Tier 1. */
static void *kai_sigalt_stack = NULL;
static int   kai_sigsegv_installed = 0;

static void kai_fiber_sigsegv_handler(int sig, siginfo_t *info, void *ucp) {
    (void) ucp;
    KaiFiber *f = kai_active_fiber;
    if (f && f->stack_base && info && info->si_addr) {
        char *guard_lo = (char *) f->stack_base;
        char *guard_hi = guard_lo + kai_page_size();
        char *addr     = (char *) info->si_addr;
        if (addr >= guard_lo && addr < guard_hi) {
            char buf[96];
            int n = snprintf(buf, sizeof(buf),
                             "kai: fiber stack overflow at %p\n", (void *) f);
            if (n > 0) {
                ssize_t w = write(2, buf, (size_t) n);
                (void) w;
            }
        }
    }
    /* Re-raise with default disposition so the process terminates with
     * the original signal — preserving the standard SIGSEGV exit
     * status for callers (shell `$?` = 139) without swallowing
     * unrelated faults. */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void kai_install_fiber_sigsegv_handler(void) {
    if (kai_sigsegv_installed) return;
    kai_sigsegv_installed = 1;

    size_t altsize = (size_t) SIGSTKSZ;
    if (altsize < 32 * 1024) altsize = 32 * 1024;
    kai_sigalt_stack = malloc(altsize);
    if (kai_sigalt_stack) {
        stack_t ss;
        ss.ss_sp    = kai_sigalt_stack;
        ss.ss_size  = altsize;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = kai_fiber_sigsegv_handler;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
}

/* =================================================================
 * Phase R1 reactor — issue #611
 * =================================================================
 *
 * Single-threaded readiness reactor wired into the cooperative
 * scheduler. Three surfaces park the calling fiber instead of the
 * OS thread:
 *   - Spawn.sleep(ns)        — timer wheel keyed on CLOCK_MONOTONIC.
 *   - File defaults          — thread-pool offload (4 workers).
 *   - Process.wait(child)    — SIGCHLD self-pipe + pid waiter map.
 *
 * Wait primitive is `poll()` on two self-pipes (one for SIGCHLD,
 * one for file-pool completions) plus a deadline-derived timeout.
 * The kqueue/epoll path the parent issue (#474) sketched is queued
 * for R2 when TCP sockets need readiness notifications; R1's three
 * surfaces never wait directly on regular FDs, so the simpler
 * `poll()` shape covers them portably without per-platform
 * branching.
 *
 * Sockets (`NetTcp`) intentionally stay on the blocking syscall
 * path until R2 ships in Orongo together with the Cancel redesign.
 * The `NetTcp` runtime note in this file is left untouched and the
 * `docs/effects-stdlib.md` sidebar for the surface continues to
 * advertise the blocking shape.
 */

/* `kai_sched_unpark` is forward-declared earlier (right after the
 * KaiFiber typedef) so the reactor drain helpers below can wake
 * promoted fibers without needing a second prototype. */

/* Self-pipe halves. The SIGCHLD handler writes one byte to
 * kai_reactor_sigchld_pipe[1] from signal context (write(2) is
 * async-signal-safe per POSIX). The reactor poll watches the read
 * half. The file-pool worker threads write one byte to
 * kai_reactor_filepool_pipe[1] on completion to wake the main
 * thread from poll(). Both pipes are O_NONBLOCK so neither writer
 * ever blocks; the reactor drains them with read() in a loop. */
static int kai_reactor_sigchld_pipe[2]  = { -1, -1 };
static int kai_reactor_filepool_pipe[2] = { -1, -1 };

/* Issue #671 — Phase R4 reactor: Signal effect self-pipe. The
 * sa_handler installed by `signal_on` writes the signo to
 * kai_reactor_signal_pipe[1] from signal context (one byte per
 * delivery; write(2) is async-signal-safe per POSIX). The reactor
 * poll watches the read half and drains it on wake, mapping signo
 * → variant and waking the parked waiter. Replaces the v1
 * sigwait body of `kai_default_signal_await` which blocked the
 * entire OS thread. */
static int kai_reactor_signal_pipe[2]   = { -1, -1 };

/* Sorted timer-wheel head (intrusive list of parked fibers chained
 * through f->reactor_next, ordered by ascending deadline). Insertion
 * is O(n); for v1 with handfuls of concurrent sleepers this stays
 * well under the noise floor of poll() itself. A heap is queued for
 * Orongo if the wheel ever shows up on a profile. */
static KaiFiber *kai_reactor_timer_head = NULL;

/* Process-wait map and file-pool waiter list. Both are intrusive
 * single-linked through f->reactor_next, so a fiber can sit on at
 * most one reactor structure at a time (asserted by the parking
 * call sites — a fiber awaiting a pid cannot simultaneously sleep
 * or sit on a file-pool completion). */
static KaiFiber *kai_reactor_pid_waiters     = NULL;
static KaiFiber *kai_reactor_filepool_waiters = NULL;

/* Issue #620 — Phase R3 reactor: stdin slot. Singleton because
 * STDIN_FILENO is process-shared; multiple fibers reading the same
 * pipe concurrently is a logic bug (the bytes would shred). The
 * parking op rejects with a clear panic if a second fiber tries.
 * Wake source is POLLIN on fd 0, drained alongside the SIGCHLD /
 * file-pool self-pipes by `kai_reactor_wait`. */
static KaiFiber *kai_reactor_stdin_waiter = NULL;
static int       kai_reactor_stdin_orig_flags = -1;

/* Issue #630 — Phase R2 reactor: per-direction socket waiter lists.
 * One fiber-per-(fd, direction); the same fd may simultaneously have
 * a reader and a writer parked (rare in v1 — typical HTTP server
 * fibers serialise send/recv on a Conn — but it is the correct
 * semantics for full-duplex sockets and costs nothing to support).
 * Each list is intrusive through `f->reactor_next`; the fd lives in
 * `f->reactor_wait_pid` (the slot doubles as "what are we waiting
 * for"; pid waiters and socket waiters are mutually exclusive). */
static KaiFiber *kai_reactor_socket_read_waiters  = NULL;
static KaiFiber *kai_reactor_socket_write_waiters = NULL;

/* Issue #671 — Phase R4 reactor: Signal waiter slot. Singleton
 * because only one fiber can sit on `Signal.await()` at a time —
 * the signo arrives in the self-pipe regardless of which fiber
 * is waiting, so multiple waiters would race over the byte. A
 * second concurrent `signal_await` panics with a clear message
 * (mirrors the stdin-multiplex panic from R3). Wake source is
 * POLLIN on `kai_reactor_signal_pipe[0]`, drained alongside the
 * SIGCHLD / file-pool / stdin pipes by `kai_reactor_wait`. The
 * delivered signo is parked in `f->reactor_data` (a void * slot)
 * so the await handler can rebuild the variant on resume. */
static KaiFiber *kai_reactor_signal_waiter = NULL;

/* Aggregate count of fibers parked on any reactor structure.
 * kai_sched_park reads this to decide between "no one can wake us
 * up — deadlock" (count == 0) and "block on the reactor until a
 * timer/SIGCHLD/file-pool event arrives" (count > 0). */
static int kai_reactor_parked_count = 0;

/* File-pool work item. Each fiber-side park allocates one of these
 * on the calling fiber's stack (lifetime = until wake) and pushes
 * onto kai_filepool_queue. A worker thread pops, invokes `work` on
 * `arg`, stores the return value in `result`, and writes one byte
 * to the completion pipe so the scheduler can pick the waiter up.
 *
 * `arg` is owned by the calling fiber for the duration of the work
 * (the worker only reads it); `result` is published by the worker
 * for the calling fiber to consume on resume. The typedef alias
 * sits up next to the reactor forward decls; only the struct
 * definition lives here. */
struct KaiFilepoolItem {
    KaiValue *(*work)(void *arg);
    void            *arg;
    KaiValue        *result;
    KaiFiber        *waiter;
    KaiFilepoolItem *queue_next;
};

static pthread_mutex_t  kai_filepool_mu    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   kai_filepool_cv    = PTHREAD_COND_INITIALIZER;
static KaiFilepoolItem *kai_filepool_q_head = NULL;
static KaiFilepoolItem *kai_filepool_q_tail = NULL;
static int              kai_filepool_started = 0;
#define KAI_FILEPOOL_WORKERS 4
static pthread_t        kai_filepool_threads[KAI_FILEPOOL_WORKERS];

/* SIGCHLD delivery slot. The handler does the minimum allowed
 * inside signal context: write a single byte to the pipe. The
 * scheduler's reactor drain reaps every child that has terminated
 * via waitpid(-1, ..., WNOHANG) and wakes the matching fiber from
 * kai_reactor_pid_waiters. */
static void kai_reactor_sigchld_handler(int sig) {
    (void) sig;
    /* write(2) is the only stdio call the SIGCHLD handler issues —
     * it is on POSIX's async-signal-safe list and the destination
     * pipe is O_NONBLOCK, so EAGAIN simply means "byte already
     * pending, the reactor will drain on next wake". */
    unsigned char b = 1;
    int saved = errno;
    if (kai_reactor_sigchld_pipe[1] >= 0) {
        ssize_t w = write(kai_reactor_sigchld_pipe[1], &b, 1);
        (void) w;
    }
    errno = saved;
}

/* Issue #671 — Phase R4: Signal-effect handler. Writes the signo
 * to the signal self-pipe so the reactor can promote the parked
 * `Signal.await()` fiber on the next poll wake. The signo fits in
 * the bottom byte (kai_signal_entries only ever contains values
 * ≤ SIGUSR2 == 31 on every POSIX system we target). write(2) and
 * the cast to unsigned char are async-signal-safe. */
static void kai_reactor_signal_handler(int sig) {
    int saved = errno;
    if (kai_reactor_signal_pipe[1] >= 0 && sig > 0 && sig <= 255) {
        unsigned char b = (unsigned char) sig;
        ssize_t w = write(kai_reactor_signal_pipe[1], &b, 1);
        (void) w;
    }
    errno = saved;
}

/* Monotonic clock helper. Used by the timer wheel for sleep
 * deadlines so wall-clock skew (NTP step, RTC adjust) does not
 * leak into Spawn.sleep semantics. */
static uint64_t kai_reactor_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        /* clock_gettime on CLOCK_MONOTONIC is guaranteed by POSIX;
         * any failure is a kernel bug. Fall back to zero so a sleep
         * still terminates rather than spinning. */
        return 0;
    }
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Sorted-insert into the timer wheel. O(n) over concurrent
 * sleepers; expected n is small in v1. */
static void kai_reactor_timer_insert(KaiFiber *f) {
    f->reactor_next = NULL;
    if (!kai_reactor_timer_head ||
        f->reactor_deadline_ns < kai_reactor_timer_head->reactor_deadline_ns) {
        f->reactor_next = kai_reactor_timer_head;
        kai_reactor_timer_head = f;
        return;
    }
    KaiFiber *c = kai_reactor_timer_head;
    while (c->reactor_next &&
           c->reactor_next->reactor_deadline_ns <= f->reactor_deadline_ns) {
        c = c->reactor_next;
    }
    f->reactor_next = c->reactor_next;
    c->reactor_next = f;
}

/* Pop every fiber whose deadline is <= `now` from the head and
 * promote it to READY. Returns the number woken. */
static int kai_reactor_timer_drain(uint64_t now) {
    int woken = 0;
    while (kai_reactor_timer_head &&
           kai_reactor_timer_head->reactor_deadline_ns <= now) {
        KaiFiber *f = kai_reactor_timer_head;
        kai_reactor_timer_head = f->reactor_next;
        f->reactor_next = NULL;
        f->reactor_deadline_ns = 0;
        kai_reactor_parked_count--;
        kai_sched_unpark(f);
        woken++;
    }
    return woken;
}

/* Splice `f` out of the timer wheel if present. Returns 1 if it was
 * found (and unparks accounting reconciled), 0 if absent. The dual-park
 * receive uses this to disarm its deadline timer once a message wakes
 * it first, so a later drain cannot wake a fiber that already returned. */
static int kai_reactor_timer_remove(KaiFiber *f) {
    KaiFiber **link = &kai_reactor_timer_head;
    while (*link) {
        if (*link == f) {
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_deadline_ns = 0;
            kai_reactor_parked_count--;
            return 1;
        }
        link = &(*link)->reactor_next;
    }
    return 0;
}

/* Drain SIGCHLD self-pipe and waitpid(-1, ..., WNOHANG) until no
 * more children have terminated, waking the fiber parked on each
 * pid. Idempotent — safe to call when no children are pending. */
static int kai_reactor_sigchld_drain(void) {
    if (kai_reactor_sigchld_pipe[0] < 0) return 0;
    /* Drain the pipe (its content is just a wake notification —
     * the real state is in the kernel's child table). */
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(kai_reactor_sigchld_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
    }
    int woken = 0;
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;  /* 0 = none ready, -1 = ECHILD/EINTR */
        KaiFiber **link = &kai_reactor_pid_waiters;
        while (*link) {
            if ((*link)->reactor_wait_pid == (int) pid) {
                KaiFiber *f = *link;
                *link = f->reactor_next;
                f->reactor_next = NULL;
                f->reactor_wait_status = status;
                /* Leave reactor_wait_pid intact so the wait op
                 * can confirm it matches on resume; clear elsewhere. */
                kai_reactor_parked_count--;
                kai_sched_unpark(f);
                woken++;
                break;
            }
            link = &(*link)->reactor_next;
        }
        /* If no fiber was parked on this pid the exit status is
         * lost. v1 contract: callers always spawn-then-wait. A
         * future "wait_any" or detached spawn would need a small
         * pending-exits buffer here; not on the R1 critical path. */
    }
    return woken;
}

/* Drain the file-pool completion pipe and promote every fiber whose
 * work item just completed. */
static int kai_reactor_filepool_drain(void) {
    if (kai_reactor_filepool_pipe[0] < 0) return 0;
    /* Empty the pipe; the real signal is in each item's `result`
     * slot being non-NULL (workers store the result before writing
     * the byte). Items waiting for promotion are still in
     * kai_reactor_filepool_waiters. */
    for (;;) {
        unsigned char buf[64];
        ssize_t n = read(kai_reactor_filepool_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
    }
    int woken = 0;
    KaiFiber **link = &kai_reactor_filepool_waiters;
    while (*link) {
        KaiFiber *f = *link;
        KaiFilepoolItem *item = (KaiFilepoolItem *) f->reactor_data;
        if (item && item->result != (KaiValue *) NULL) {
            *link = f->reactor_next;
            f->reactor_next = NULL;
            kai_reactor_parked_count--;
            kai_sched_unpark(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}

/* Issue #671 — Phase R4: drain the Signal self-pipe and promote
 * the parked `Signal.await()` waiter (if any). The first byte
 * pulled is the delivered signo; subsequent bytes mean another
 * signal arrived while the first one was still queued — those
 * are discarded under v1's "single waiter, single fire" contract.
 * If no fiber is parked at drain time the signo is also discarded;
 * `signal_await` documents the race (a signal that arrived before
 * the first await call is lost). The matching `kai_reactor_signal_*`
 * design comment in stage0/runtime.h covers the trade-off. */
static int kai_reactor_signal_drain(void) {
    if (kai_reactor_signal_pipe[0] < 0) return 0;
    int signo = 0;
    unsigned char buf[64];
    for (;;) {
        ssize_t n = read(kai_reactor_signal_pipe[0], buf, sizeof(buf));
        if (n <= 0) break;
        /* Keep the most recent signo from this batch; v1 collapses
         * concurrent deliveries to one wake. */
        if (n > 0) signo = (int) buf[n - 1];
    }
    if (signo == 0)                 return 0;
    if (!kai_reactor_signal_waiter) return 0;
    KaiFiber *f = kai_reactor_signal_waiter;
    kai_reactor_signal_waiter = NULL;
    /* Stash the signo in reactor_wait_status so the await handler
     * can recover it on resume. Re-use of the slot is safe: the
     * fiber is parked on a singleton waiter, never simultaneously
     * on a pid / socket waiter. */
    f->reactor_wait_status = signo;
    kai_reactor_parked_count--;
    kai_sched_unpark(f);
    return 1;
}

/* File-pool worker loop. Pops items off the FIFO queue, runs the
 * work function on the worker thread (so the blocking syscall stays
 * off the scheduler thread), publishes the result, and writes one
 * byte to the completion pipe to wake the scheduler. */
static void *kai_filepool_worker(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&kai_filepool_mu);
        while (!kai_filepool_q_head) {
            pthread_cond_wait(&kai_filepool_cv, &kai_filepool_mu);
        }
        KaiFilepoolItem *item = kai_filepool_q_head;
        kai_filepool_q_head = item->queue_next;
        if (!kai_filepool_q_head) kai_filepool_q_tail = NULL;
        pthread_mutex_unlock(&kai_filepool_mu);

        /* Sentinel item with NULL `work` signals shutdown — not
         * exercised in v1 (the runtime never tears down) but kept
         * symmetric with the queue protocol. */
        if (!item->work) return NULL;

        KaiValue *r = item->work(item->arg);

        /* Publish the result, then notify. The order matters: the
         * scheduler reads `result` after the byte arrives, so the
         * store must be visible first. v1 single-CPU x86/arm64
         * provides release ordering on aligned word stores, but a
         * pthread_mutex round-trip gives us the same guarantee
         * portably. */
        pthread_mutex_lock(&kai_filepool_mu);
        item->result = r ? r : kai_unit();
        pthread_mutex_unlock(&kai_filepool_mu);

        unsigned char b = 1;
        if (kai_reactor_filepool_pipe[1] >= 0) {
            ssize_t w = write(kai_reactor_filepool_pipe[1], &b, 1);
            (void) w;
        }
    }
}

/* Issue #620 — restore stdin's original flags on process exit. The
 * runtime flips fd 0 to O_NONBLOCK once the first stdin op runs;
 * leaving the shell's stdin in non-blocking mode after exit is a
 * subtle, hard-to-diagnose footgun (tools downstream of the kaikai
 * program would see EAGAIN on every read). atexit guarantees this
 * runs on normal termination and on `exit()` calls. */
static void kai_reactor_stdin_restore(void) {
    if (kai_reactor_stdin_orig_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, kai_reactor_stdin_orig_flags);
        kai_reactor_stdin_orig_flags = -1;
    }
}

/* Issue #620 — set fd 0 to O_NONBLOCK once per process. Saves the
 * original flags into kai_reactor_stdin_orig_flags so atexit can
 * restore them. No-op on subsequent calls. If F_GETFL fails (eg. fd
 * 0 closed by the parent program), the function is a no-op and the
 * stdin parking path will surface the failure as a Fail/error. */
static void kai_reactor_stdin_set_nonblocking(void) {
    if (kai_reactor_stdin_orig_flags >= 0) return;
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl < 0) return;
    kai_reactor_stdin_orig_flags = fl;
    fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    atexit(kai_reactor_stdin_restore);
}

/* Lazy initialisation of the reactor on first use. Idempotent;
 * safe to call from every parking site. Installs SIGCHLD additively
 * (only if no existing handler is registered) so the stack-guard
 * SIGSEGV path and any future signal users continue to operate.
 *
 * The file-pool workers (4 OS threads) are deferred to the first
 * actual file op via kai_reactor_init_filepool — sleep-only and
 * process-only workloads should not pay the pthread_create cost. */
static void kai_reactor_init_filepool(void);
static void kai_reactor_init(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    /* Self-pipes for the three wake sources (SIGCHLD, file-pool,
     * Signal R4). O_NONBLOCK so handlers and worker threads never
     * block; O_CLOEXEC so a forked child does not inherit them. */
    if (pipe(kai_reactor_sigchld_pipe)  != 0 ||
        pipe(kai_reactor_filepool_pipe) != 0 ||
        pipe(kai_reactor_signal_pipe)   != 0) {
        fprintf(stderr, "kai: reactor pipe() failed: %s\n", strerror(errno));
        exit(1);
    }
    for (int i = 0; i < 3; i++) {
        int fds[3][2] = {
            { kai_reactor_sigchld_pipe[0],  kai_reactor_sigchld_pipe[1]  },
            { kai_reactor_filepool_pipe[0], kai_reactor_filepool_pipe[1] },
            { kai_reactor_signal_pipe[0],   kai_reactor_signal_pipe[1]   },
        };
        for (int p = 0; p < 2; p++) {
            int fd = fds[i][p];
            int fl = fcntl(fd, F_GETFL, 0);
            if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int cl = fcntl(fd, F_GETFD, 0);
            if (cl != -1) fcntl(fd, F_SETFD, cl | FD_CLOEXEC);
        }
    }

    /* Install SIGCHLD additively. The stack-guard handler grabs
     * SIGSEGV/SIGBUS; we are explicit about touching only SIGCHLD
     * to avoid stomping on it. Refuse to install if SIGCHLD is
     * already taken by user code; the runtime exits with a clear
     * diagnostic rather than silently overriding. */
    struct sigaction old;
    if (sigaction(SIGCHLD, NULL, &old) == 0) {
        if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            fprintf(stderr,
                "kai: reactor cannot install SIGCHLD — slot already taken\n");
            exit(1);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = kai_reactor_sigchld_handler;
    sigemptyset(&sa.sa_mask);
    /* SA_NOCLDSTOP so we are not woken for stop/continue traffic —
     * only terminal child exits matter. SA_RESTART is intentionally
     * NOT set: the scheduler's poll() must return early on EINTR so
     * the wake path is observed even when the self-pipe write loses
     * the race with the kernel's signal delivery. The worker
     * threads block SIGCHLD via their thread mask (see below), so
     * the absence of SA_RESTART does not affect their blocking
     * reads/writes. */
    sa.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* Spin up the 4-worker file pool on the first file op. SIGCHLD is
 * blocked in every worker thread's mask so the signal always lands
 * on the scheduler (main) thread, which is the one draining the
 * self-pipe in kai_reactor_wait. The pool runs detached for the
 * lifetime of the process; the OS reclaims the threads on exit. */
static void kai_reactor_init_filepool(void) {
    if (kai_filepool_started) return;
    kai_filepool_started = 1;
    sigset_t block_set, prev_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_set, &prev_set);
    for (int i = 0; i < KAI_FILEPOOL_WORKERS; i++) {
        if (pthread_create(&kai_filepool_threads[i], NULL,
                           kai_filepool_worker, NULL) != 0) {
            fprintf(stderr, "kai: reactor pthread_create failed: %s\n",
                    strerror(errno));
            exit(1);
        }
        pthread_detach(kai_filepool_threads[i]);
    }
    pthread_sigmask(SIG_SETMASK, &prev_set, NULL);
}

/* Submit a unit of work to the file-pool. The caller (a parking
 * file op handler) supplies `work` + `arg`, then parks itself on
 * kai_reactor_filepool_waiters and yields. The worker invokes
 * `work(arg)`, stores the KaiValue * result in `item->result`,
 * and wakes the scheduler. The item itself lives on the caller's
 * fiber stack — no heap allocation. */
static void kai_filepool_submit(KaiFilepoolItem *item) {
    pthread_mutex_lock(&kai_filepool_mu);
    item->queue_next = NULL;
    if (kai_filepool_q_tail) {
        kai_filepool_q_tail->queue_next = item;
    } else {
        kai_filepool_q_head = item;
    }
    kai_filepool_q_tail = item;
    pthread_cond_signal(&kai_filepool_cv);
    pthread_mutex_unlock(&kai_filepool_mu);
}

/* Public entry points used by the Clock / Process / File default
 * handlers. Each is a thin wrapper that links the calling fiber
 * into the appropriate reactor structure, bumps the parked-count,
 * and yields via kai_sched_park (which falls into kai_reactor_wait
 * once the run queue empties). On resume the reactor drain has
 * already cleared the fiber's reactor_* slots and unparked it. */
static void kai_reactor_park_timer(KaiFiber *f, uint64_t deadline_ns) {
    f->reactor_deadline_ns = deadline_ns;
    kai_reactor_timer_insert(f);
    kai_reactor_parked_count++;
    kai_sched_park();
}

static void kai_reactor_park_pid(KaiFiber *f, int pid) {
    f->reactor_wait_pid    = pid;
    f->reactor_wait_status = 0;
    /* Push onto the head; pid lookup walks the list so order is
     * irrelevant. The drain helper splices the matching node out. */
    f->reactor_next = kai_reactor_pid_waiters;
    kai_reactor_pid_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Issue #620 — Phase R3: park `f` on the singleton stdin slot.
 * Returns 0 on success and -1 if another fiber already holds the
 * slot (the caller should treat that as "concurrent stdin readers
 * — undefined" and panic). The slot is cleared by `kai_reactor_wait`
 * when POLLIN / POLLHUP / POLLERR fires on STDIN_FILENO; on resume
 * the parking site simply retries its read(). */
static int kai_reactor_park_stdin(KaiFiber *f) {
    if (kai_reactor_stdin_waiter != NULL) return -1;
    kai_reactor_stdin_waiter = f;
    kai_reactor_parked_count++;
    kai_sched_park();
    return 0;
}

/* Issue #671 — Phase R4: park `f` on the singleton Signal waiter
 * slot. Returns 0 on success, -1 if another fiber is already
 * parked (the caller panics with a clear diagnostic, same shape
 * as the R3 stdin contract). On resume the delivered signo lives
 * in `f->reactor_wait_status`; the await handler maps it back to
 * the matching variant. */
static int kai_reactor_park_signal(KaiFiber *f) {
    if (kai_reactor_signal_waiter != NULL) return -1;
    f->reactor_wait_status = 0;
    kai_reactor_signal_waiter = f;
    kai_reactor_parked_count++;
    kai_sched_park();
    return 0;
}

/* Issue #630 — Phase R2: set O_NONBLOCK on a socket fd. Idempotent;
 * safe to call once per fd creation site. We do NOT save/restore the
 * original flags (unlike stdin) because the fd was just opened by us
 * — no caller cares about its pre-flag state. */
static void kai_socket_set_nonblock(int fd) {
    if (fd < 0) return;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return;
    if (!(fl & O_NONBLOCK)) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/* Issue #630 — Phase R2: park `f` waiting for read-readiness on
 * `fd` (accept on a listener, recv on a connection). Pushes the
 * fiber onto the socket_read_waiters list, stashes the fd in
 * reactor_wait_pid (slot is repurposed; pid waiters and socket
 * waiters are mutually exclusive), bumps the parked-count, and
 * yields. On resume the drain helper has spliced the fiber out;
 * the caller retries its non-blocking read(). */
static void kai_reactor_park_socket_read(KaiFiber *f, int fd) {
    f->reactor_wait_pid    = fd;
    f->reactor_wait_status = 0;
    f->reactor_next = kai_reactor_socket_read_waiters;
    kai_reactor_socket_read_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Issue #630 — Phase R2: park `f` waiting for write-readiness on
 * `fd` (connect handshake completion, send when the kernel buffer is
 * full). Symmetric with kai_reactor_park_socket_read on the
 * socket_write_waiters list. */
static void kai_reactor_park_socket_write(KaiFiber *f, int fd) {
    f->reactor_wait_pid    = fd;
    f->reactor_wait_status = 0;
    f->reactor_next = kai_reactor_socket_write_waiters;
    kai_reactor_socket_write_waiters = f;
    kai_reactor_parked_count++;
    kai_sched_park();
}

/* Submit `work(arg)` to the file-pool worker queue and park the
 * caller until completion. Returns the worker's KaiValue *result.
 * The KaiFilepoolItem lives on the caller's fiber stack — safe
 * because the parked fiber's stack is preserved until resume. */
static KaiValue *kai_reactor_run_in_pool(KaiValue *(*work)(void *), void *arg) {
    kai_reactor_init_filepool();
    KaiFilepoolItem item;
    item.work       = work;
    item.arg        = arg;
    item.result     = NULL;
    item.waiter     = kai_current_fiber();
    item.queue_next = NULL;

    KaiFiber *me = kai_current_fiber();
    me->reactor_data = &item;
    /* Queue order is irrelevant for the waiter list; insert at head. */
    me->reactor_next = kai_reactor_filepool_waiters;
    kai_reactor_filepool_waiters = me;
    kai_reactor_parked_count++;

    kai_filepool_submit(&item);
    kai_sched_park();

    /* On resume the drain helper has spliced us out of
     * kai_reactor_filepool_waiters. The result slot is set by the
     * worker prior to the pipe write. */
    KaiValue *r = item.result;
    me->reactor_data = NULL;
    return r ? r : kai_unit();
}

/* Issue #630 — Phase R2: count the live socket-waiter fds in each
 * direction. Two fibers parked on the same fd in the same direction
 * is impossible in v1 (every Conn / Listener belongs to a single
 * fiber under the per-fiber-arena model). Counting unique fds is
 * therefore equivalent to counting waiters. */
static int kai_reactor_count_socket_waiters(KaiFiber *head) {
    int n = 0;
    for (KaiFiber *f = head; f; f = f->reactor_next) n++;
    return n;
}

/* Issue #630 — Phase R2: drain one socket-direction waiter list.
 * For every fiber whose fd shows up in `pfds[i].revents` with
 * POLLIN/POLLOUT/POLLHUP/POLLERR set, splice it out and unpark.
 * The handler at the park site will retry its non-blocking syscall
 * and either succeed, EOF, or re-park if the kernel buffer drained
 * between wake and retry (spurious wake — POSIX permits it). */
static int kai_reactor_socket_drain(KaiFiber **head_ptr, struct pollfd *pfds,
                                    int nfds, short ready_mask) {
    int woken = 0;
    KaiFiber **link = head_ptr;
    while (*link) {
        int fd = (*link)->reactor_wait_pid;
        /* OR every pfds entry's revents for this fd. The same fd can
         * appear in the poll set multiple times (full-duplex sockets
         * with both read and write waiters, or two readers on a
         * shared listener — accept() distributes connections across
         * them). Aggregating revents avoids missing a wake when the
         * ready entry is not the first match. */
        short revents = 0;
        for (int i = 0; i < nfds; i++) {
            if (pfds[i].fd == fd) revents |= pfds[i].revents;
        }
        if (revents & (ready_mask | POLLHUP | POLLERR | POLLNVAL)) {
            KaiFiber *f = *link;
            *link = f->reactor_next;
            f->reactor_next = NULL;
            f->reactor_wait_pid = 0;
            kai_reactor_parked_count--;
            kai_sched_unpark(f);
            woken++;
        } else {
            link = &(*link)->reactor_next;
        }
    }
    return woken;
}

/* Block in poll() until either the SIGCHLD pipe or the file-pool
 * completion pipe fires, a socket fd becomes ready, or the next
 * timer deadline arrives. Promotes every newly-ready fiber to the
 * run queue. Called by kai_sched_park when the ready queue is empty
 * but reactor waiters exist — the dispatch loop's substitute for a
 * dedicated event loop thread. */
static void kai_reactor_wait(void) {
    /* Compute the timeout in ms (poll's resolution). A negative
     * timeout is "wait forever"; a zero timeout polls. */
    int timeout_ms = -1;
    if (kai_reactor_timer_head) {
        uint64_t now = kai_reactor_now_ns();
        uint64_t dl  = kai_reactor_timer_head->reactor_deadline_ns;
        if (dl <= now) {
            timeout_ms = 0;
        } else {
            uint64_t diff_ns = dl - now;
            uint64_t ms = (diff_ns + 999999ULL) / 1000000ULL;  /* ceil */
            if (ms > (uint64_t) INT_MAX) ms = (uint64_t) INT_MAX;
            timeout_ms = (int) ms;
        }
    }

    /* Size the pfds array: 2 self-pipes + optional stdin + N
     * socket-read + M socket-write. Use a small stack buffer for the
     * common case (no sockets) and fall back to a heap alloc when
     * more than ~16 fds are live. */
    int nread  = kai_reactor_count_socket_waiters(kai_reactor_socket_read_waiters);
    int nwrite = kai_reactor_count_socket_waiters(kai_reactor_socket_write_waiters);
    /* 3 self-pipes (sigchld + filepool + signal) + optional stdin
     * (R3) + optional signal-waiter pipe (R4 — same fd as the
     * self-pipe, but kept in the count for clarity). socket waiters
     * grow the set per (fd, direction). */
    int max_fds = 3 + 1 + nread + nwrite;
    struct pollfd  stack_pfds[16];
    struct pollfd *pfds = stack_pfds;
    int heap_alloced = 0;
    if (max_fds > (int) (sizeof(stack_pfds) / sizeof(stack_pfds[0]))) {
        pfds = (struct pollfd *) malloc((size_t) max_fds * sizeof(*pfds));
        if (!pfds) {
            fprintf(stderr, "kai: reactor pfds malloc failed\n");
            exit(1);
        }
        heap_alloced = 1;
    }

    int nfds = 0;
    if (kai_reactor_sigchld_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_sigchld_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    if (kai_reactor_filepool_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_filepool_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #671 — Phase R4: signal self-pipe stays in the poll
     * set for the lifetime of the process. A signal that arrives
     * with no parked waiter is dropped by the drain — that races
     * the v1 sigwait body exactly the same way (an unblocked signal
     * arriving before sigwait() was lost too); the new path adds
     * no new strand-against-handler hazard. */
    if (kai_reactor_signal_pipe[0] >= 0) {
        pfds[nfds].fd = kai_reactor_signal_pipe[0];
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #620 — Phase R3: only register stdin in the poll set
     * while a fiber is parked on it. Otherwise we would wake on
     * every keystroke even when no one is reading, burn CPU, and
     * have no waiter to promote. */
    if (kai_reactor_stdin_waiter != NULL) {
        pfds[nfds].fd = STDIN_FILENO;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    /* Issue #630 — Phase R2: register every socket waiter's fd. If
     * the same fd shows up in both directions (a fiber reading on a
     * fd while another writes to it — rare in v1 but legal), the fd
     * appears twice in the poll set with disjoint event masks; POSIX
     * permits this and reports revents per-entry. */
    for (KaiFiber *f = kai_reactor_socket_read_waiters; f; f = f->reactor_next) {
        pfds[nfds].fd = f->reactor_wait_pid;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }
    for (KaiFiber *f = kai_reactor_socket_write_waiters; f; f = f->reactor_next) {
        pfds[nfds].fd = f->reactor_wait_pid;
        pfds[nfds].events = POLLOUT;
        pfds[nfds].revents = 0;
        nfds++;
    }
    int rc = poll(pfds, (nfds_t) nfds, timeout_ms);
    if (rc < 0 && errno != EINTR) {
        fprintf(stderr, "kai: reactor poll() failed: %s\n", strerror(errno));
        exit(1);
    }

    /* Drain in a fixed order. Even on EINTR (rc < 0) the timer
     * wheel must be drained because a stray signal could have
     * coincided with a deadline expiry. */
    uint64_t now = kai_reactor_now_ns();
    kai_reactor_timer_drain(now);
    if (rc > 0) {
        /* Self-pipe and stdin paths first (these compare against the
         * fixed fds we know up front). */
        for (int i = 0; i < nfds; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (pfds[i].fd == kai_reactor_sigchld_pipe[0]) {
                kai_reactor_sigchld_drain();
            } else if (pfds[i].fd == kai_reactor_filepool_pipe[0]) {
                kai_reactor_filepool_drain();
            } else if (pfds[i].fd == kai_reactor_signal_pipe[0]) {
                kai_reactor_signal_drain();
            } else if (pfds[i].fd == STDIN_FILENO &&
                       kai_reactor_stdin_waiter != NULL) {
                /* Issue #620 — readiness event on stdin: promote
                 * the parked fiber. POLLHUP / POLLERR also count
                 * as wakeups so a closed pipe (EOF) does not
                 * strand the waiter forever. */
                KaiFiber *f = kai_reactor_stdin_waiter;
                kai_reactor_stdin_waiter = NULL;
                kai_reactor_parked_count--;
                kai_sched_unpark(f);
            }
        }
        /* Socket waiters: separate drain pass because the same fd
         * may appear in both directions (and the read/write masks
         * are disjoint). Each drain handles only the matching list. */
        kai_reactor_socket_drain(&kai_reactor_socket_read_waiters,  pfds, nfds, POLLIN);
        kai_reactor_socket_drain(&kai_reactor_socket_write_waiters, pfds, nfds, POLLOUT);
    }
    /* A SIGCHLD delivered before we entered poll() may not appear
     * in revents (the signal handler ran but the byte arrived
     * after the kernel snapshot). Always attempt a non-blocking
     * waitpid drain too so children that exited during the
     * micro-window before poll() do not strand their fibers. */
    if (kai_reactor_pid_waiters) {
        kai_reactor_sigchld_drain();
    }

    if (heap_alloced) free(pfds);
}

static void kai_sched_enqueue(KaiFiber *f) {
    f->sched_next = NULL;
    if (kai_ready_tail) {
        kai_ready_tail->sched_next = f;
    } else {
        kai_ready_head = f;
    }
    kai_ready_tail = f;
}

static KaiFiber *kai_sched_dequeue(void) {
    KaiFiber *f = kai_ready_head;
    if (!f) return NULL;
    kai_ready_head = f->sched_next;
    if (!kai_ready_head) kai_ready_tail = NULL;
    f->sched_next = NULL;
    return f;
}

/* Forward decl: the trampoline drives a fiber's body and walks its
 * awaiter chain on completion. Defined below so it can call the
 * scheduler primitives + kai_apply. */
static void kai_fiber_trampoline(void);

/* Phase 5 forward decl: link propagation runs in the trampoline's
 * termination tail; the helper itself is defined alongside the
 * Link default handler further down. The reason argument distinguishes
 * Normal (DONE) from Crashed (CANCELLED) for trap-exit delivery. */
typedef enum {
    KAI_EXIT_NORMAL  = 0,
    KAI_EXIT_CRASHED = 1
} KaiExitReason;

static void kai_link_propagate_terminate(KaiFiber *self, KaiExitReason reason);
/* Tier 2 Monitor — observers learn about target's termination via a
 * single push of `target_pid` into observer->mailbox. Defined
 * alongside the Monitor default handler further down. */
static void kai_monitor_propagate_terminate(KaiFiber *self);

/* Initialise a freshly-allocated KaiFiber's ucontext + private stack.
 * Sets f->ctx so it can be a swapcontext target; fills uc_link as a
 * fallback (the trampoline always exits via setcontext, so uc_link
 * is reachable only on a runtime bug). The thunk slot must already
 * be filled by the caller before the fiber runs. */
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static void kai_fiber_init_ctx(KaiFiber *f) {
    kai_install_fiber_sigsegv_handler();
    if (getcontext(&f->ctx) != 0) {
        fprintf(stderr, "kai: getcontext failed for new fiber\n");
        exit(1);
    }
    f->stack_size = kai_fiber_stack_size();
    /* Allocate stack + one guard page below it. Stack grows down on
     * x86_64 / arm64, so the lowest address is the overflow target.
     * stack_base points at the guard; the usable region starts one
     * page above. Layout (low → high):
     *   [ guard page (PROT_NONE) | stack_size bytes (RW) ]
     * We store stack_base as the mmap base so munmap covers both;
     * stack_size remains the usable size, and total = stack_size +
     * page_size whenever we need to release the region. */
    size_t page = kai_page_size();
    size_t total = f->stack_size + page;
    void *region = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) {
        fprintf(stderr, "kai: mmap failed allocating fiber stack (%zu bytes)\n",
                total);
        exit(1);
    }
    if (mprotect(region, page, PROT_NONE) != 0) {
        fprintf(stderr, "kai: mprotect guard page failed for fiber stack\n");
        munmap(region, total);
        exit(1);
    }
    f->stack_base = region;
    f->ctx.uc_stack.ss_sp   = (char *) region + page;
    f->ctx.uc_stack.ss_size = f->stack_size;
    f->ctx.uc_link          = &kai_main_fiber.ctx;
    makecontext(&f->ctx, kai_fiber_trampoline, 0);
}

/* Yield: caller stays READY and goes back on the run queue; control
 * swaps to the head of the queue. No-op if the queue is empty (caller
 * is the only ready fiber, nothing to switch to). */
static void kai_sched_yield(void) {
    KaiFiber *current = kai_active_fiber;
    KaiFiber *next    = kai_sched_dequeue();
    if (!next) return;  /* alone — nothing to yield to */
    current->state = KAI_FIBER_READY;
    kai_sched_enqueue(current);
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    swapcontext(&current->ctx, &next->ctx);
    /* Resumed: another fiber yielded/parked back to us; the swap
     * source (current->ctx) holds the state that was just restored.
     * R4 fix — if the fiber that swapped to us was the trampoline
     * tail of a now-discarded fiber, `kai_pending_free` carries its
     * deferred struct + stack. Reap before continuing. */
    kai_drain_pending_free();
}

/* Park: caller goes PARKED, control swaps to the head of the run
 * queue. The caller must have already linked itself into a wakeup
 * list (awaiter chain, mailbox waiter list, or a reactor structure)
 * before calling park — otherwise no fiber can ever unpark it.
 *
 * Issue #611 — Phase R1 reactor integration: when the ready queue
 * is empty but at least one fiber is parked on the reactor (timer
 * wheel, pid waiter, file-pool waiter), block in `kai_reactor_wait`
 * until an event promotes someone, then redequeue. Deadlock is now
 * `ready queue empty AND reactor empty` — only that combination
 * means no path to forward progress. */
static void kai_sched_park(void) {
    KaiFiber *current = kai_active_fiber;
    /* Mark the caller PARKED up front so the reactor drain helpers
     * (which may run inside kai_reactor_wait below) can promote us
     * back to READY via kai_sched_unpark. The unpark path bails out
     * when state != PARKED, so a fiber whose state is still RUNNING
     * when its deadline fires would be lost. */
    current->state = KAI_FIBER_PARKED;
    kai_parked_count++;

    KaiFiber *next = kai_sched_dequeue();
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue();
            continue;
        }
        fprintf(stderr,
            "kai: deadlock — fiber parked with empty run queue (%d parked total)\n",
            kai_parked_count);
        exit(1);
    }
    if (next == current) {
        /* The reactor wake promoted us before we picked anyone else.
         * Skip the swapcontext (we are still on our own stack) and
         * unwind the parked accounting we just bumped. */
        current->state = KAI_FIBER_RUNNING;
        kai_parked_count--;
        /* Issue #679: also observe a sibling-triggered cancel here.
         * The unpark path from `kai_default_spawn_cancel`'s reactor
         * detach lands here when the canceller is on the same OS
         * thread frame as the reactor wake. */
        kai_check_cancel_yield_point();
        return;
    }
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    swapcontext(&current->ctx, &next->ctx);
    /* R4 fix — see kai_sched_yield: drain pending free on resume. */
    kai_drain_pending_free();
    /* Issue #679: every reactor-driven park resumes here. If a
     * sibling fiber called Spawn.cancel(self) while we were parked,
     * the reactor detach + unpark wakes us and we must observe the
     * cancel before retrying the syscall the park site wrapped.
     * The yield-point check longjmps to `cancel_pad` if the flag is
     * set; otherwise the park site's syscall retry loop continues
     * normally. */
    kai_check_cancel_yield_point();
}

/* Unpark: target PARKED → READY, enqueue at run queue tail. Caller
 * stays RUNNING (does not yield); the unparked fiber runs whenever
 * the scheduler reaches it. No-op if target is not currently
 * PARKED (defensive against double-unpark). */
static void kai_sched_unpark(KaiFiber *target) {
    if (!target || target->state != KAI_FIBER_PARKED) return;
    target->state = KAI_FIBER_READY;
    kai_parked_count--;
    kai_sched_enqueue(target);
}

/* Trampoline: the entry point makecontext installs on every spawned
 * fiber's stack. Reads kai_active_fiber to find itself (set by the
 * dispatcher who swapped in), runs the thunk, walks awaiters, and
 * hands control to the next ready fiber via setcontext.
 *
 * Phase 3 — Cancel landing: setjmp(cancel_pad) before the body runs.
 * The yield-point hook in kai_evidence_lookup* longjmps here when
 * cancel_requested fires; the second-return path skips the body and
 * marks the fiber CANCELLED instead of DONE. cancel_pad_set is the
 * gate the hook reads before attempting the longjmp.
 *
 * RC contract: f->thunk is owned by f for f's entire lifetime;
 * kai_apply consumes (#298), so the trampoline incref's the thunk
 * before each invocation to keep f->thunk's lifetime independent of
 * the call. kai_free_value's KAI_FIBER branch decrefs both thunk and
 * result when f's RC drops. */
static void kai_fiber_trampoline(void) {
    KaiFiber *self = kai_active_fiber;
    /* First entry follows a setcontext from another fiber's
     * trampoline tail or a swap from yield/park. Drain any pending
     * struct free left behind by the previous fiber's
     * `kai_decref(self->value)` before we touch our own state. */
    kai_drain_pending_free();
    if (setjmp(self->cancel_pad) == 0) {
        self->cancel_pad_set = 1;
        /* kai_apply consumes (#298): give it its own ref, keep f->thunk. */
        self->result = kai_apply(kai_incref(self->thunk), 0, NULL);
        self->cancel_pad_set = 0;
        self->state  = KAI_FIBER_DONE;
    } else {
        /* Cancel landing: the yield-point hook (or kai_default_cancel_raise)
         * longjmped here. The body did not finish; result stays NULL. */
        self->cancel_pad_set = 0;
        self->state = KAI_FIBER_CANCELLED;
    }

    /* Phase 5 — Link propagation. Walk the linked chain. For each
     * peer, behaviour depends on the peer's trap_exit flag (Tier 2):
     *   - trap_exit=0 (default): set cancel_requested (current
     *     behaviour, delivered at the peer's next yield-point hook).
     *   - trap_exit=1: push a "Normal"/"Crashed" string into the
     *     peer's mailbox so the peer can react in user code instead
     *     of being cancelled.
     * The exit reason is read from `self->state` at this point —
     * DONE → Normal, CANCELLED → Crashed (any other state would be
     * a runtime invariant violation). */
    kai_link_propagate_terminate(self,
        self->state == KAI_FIBER_DONE ? KAI_EXIT_NORMAL : KAI_EXIT_CRASHED);

    /* Tier 2 Monitor — push our pid into each observer's mailbox.
     * Observers do not get cancel_requested set; monitors are
     * unidirectional and fault-isolated. */
    kai_monitor_propagate_terminate(self);

    /* Wake awaiters. Each was parked in Spawn.await (Phase 2.3) or
     * Spawn.select (Phase 2.3). Walk the chain, clearing each
     * awaiter's next-link before unparking so a re-park does not see
     * stale links. */
    KaiFiber *a = self->awaiters_head;
    self->awaiters_head = NULL;
    while (a) {
        KaiFiber *nx = a->awaiters_next;
        a->awaiters_next = NULL;
        kai_sched_unpark(a);
        a = nx;
    }

    /* R4 fix — drop the scheduler-side incref on our wrapper, taken
     * by `kai_default_spawn_spawn` before enqueue. Pairs `enqueue`
     * with this single decref. If the user already discarded their
     * Fiber[T] handle, this brings RC to 0 and `kai_free_value`
     * defers the struct/stack free into `kai_pending_free` (we are
     * still standing on this fiber's stack); the next fiber's drain
     * hook reaps it. If awaiters or the user still hold the handle,
     * the wrapper survives this decref and is freed later by their
     * own drops. */
    if (self->value) {
        kai_decref(self->value);
    }

    /* Hand control to the next ready fiber. Awaiters we just woke
     * are now in the queue; the FIFO discipline picks the oldest
     * ready (which may be one of them, or main, or a sibling).
     *
     * Issue #611 — if the queue is empty here but reactor waiters
     * remain (timer wheel, pid map, file-pool list), block in
     * kai_reactor_wait until a wake event promotes someone before
     * declaring deadlock. */
    KaiFiber *next = kai_sched_dequeue();
    while (!next) {
        if (kai_reactor_parked_count > 0) {
            kai_reactor_wait();
            next = kai_sched_dequeue();
            continue;
        }
        fprintf(stderr,
            "kai: fiber finished with empty run queue (%d parked) — deadlock\n",
            kai_parked_count);
        exit(1);
    }
    next->state = KAI_FIBER_RUNNING;
    kai_active_fiber = next;
    setcontext(&next->ctx);
    /* setcontext does not return. */
}
#if defined(__APPLE__) || defined(__clang__)
#  pragma clang diagnostic pop
#endif

/* m8.x: default Spawn handlers backed by the cooperative scheduler.
 * spawn(thunk)        — alloc fiber + enqueue; thunk runs later.
 * await(fiber)        — park on the fiber's awaiter chain until DONE.
 * yield()             — rotate run queue (no-op if alone).
 * select([fiber])     — Phase 2 simplification: head-of-list, parking
 *                       on the head if not yet DONE. Real race +
 *                       cancel-losers semantics land in Phase 4
 *                       alongside Cancel delivery (Phase 3).
 * cancel(fiber)       — set cancel_requested; delivery is Phase 3.
 *
 * RC contract: op handlers borrow their KaiValue* args (the call site
 * decrefs after the call per Perceus). Handlers that need to retain
 * an arg `kai_incref` before storing — same pattern the m8 v1 handlers
 * used and that `kai_prelude_map` documents at runtime.h:1031-1040.
 * R1 Phase 3 flipped only the 13 primitives named in CHANGELOG v0.2.0
 * step C; effect-op handlers were not touched, so the call-site / op
 * contract stayed "callee borrows".
 *
 * Spec: docs/fibers-impl.md §*Yield-point list* and §*Trampoline*. */
static KaiValue *kai_default_spawn_yield(void *self, KaiCont *k) {
    (void) self;
    kai_sched_yield();
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_spawn_spawn(void *self, KaiValue *thunk, KaiCont *k) {
    (void) self;
    if (!thunk || thunk->tag != KAI_CLOSURE) {
        fprintf(stderr, "kai: Spawn.spawn called with non-closure value\n");
        exit(1);
    }
    KaiFiber *f = (KaiFiber *) calloc(1, sizeof(KaiFiber));
    if (!f) { fprintf(stderr, "kai: out of memory\n"); exit(1); }
    f->parent       = kai_current_fiber();
    /* incref to retain a ref the fiber owns; the caller-side ref is
     * still the caller's to manage (Perceus decrefs at call-site exit). */
    f->thunk        = kai_incref(thunk);
    /* Capabilities do not cross a spawn: a value-transportable effect
     * rides the child thunk's own evidence frame, and a fiber-local
     * effect resolves against this fiber's own disposition (cancel pad,
     * link set, mailbox, nursery), never the parent's. The child starts
     * with an empty evidence stack rather than inheriting the parent's —
     * inheriting it would carry a parent handler whose *Ev struct lives
     * on a stack frame the parent overwrites once it returns. */
    f->evidence_top          = NULL;
    kai_fiber_init_ctx(f);
    /* R4 fix — allocate the wrapper before enqueue so the scheduler
     * can hold its own incref on the value. Without this second ref a
     * `let _ = fiber_spawn(…)` discard would drop the wrapper to
     * RC=0 while the struct is still in the ready queue, and the
     * trampoline would later run on freed memory. The wrapper RC
     * therefore starts at 2: one for the caller (the user-visible
     * Fiber[T] handle) and one for the scheduler (released in the
     * trampoline's DONE/CANCELLED tail via `kai_decref(self->value)`). */
    KaiValue *v = kai_fiber_value(f);  /* RC=1, sets f->value */
    kai_incref(v);                     /* RC=2, scheduler's own ref */
    /* Issue #959 — register on the spawning fiber's innermost open
     * nursery. The scope takes its own ref (RC=3 here) so a discarded
     * `Fiber[T]` handle cannot free the wrapper before `nursery_exit`
     * joins this child; the scope releases it during the join walk. */
    KaiNursery *scope = kai_current_fiber()->nursery_top;
    if (scope) {
        kai_incref(v);
        f->scope_sibling_next = scope->children_head;
        scope->children_head  = f;
    }
    f->state = KAI_FIBER_READY;
    kai_sched_enqueue(f);
    return kai_cont_resume(k, v);
}

static KaiValue *kai_default_spawn_await(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (!fib_v || fib_v->tag != KAI_FIBER || !fib_v->as.fib) {
        fprintf(stderr, "kai: Spawn.await called on non-fiber value\n");
        exit(1);
    }
    KaiFiber *f = fib_v->as.fib;
    /* Issue #679: a fiber cancelled mid-flight (e.g. cancelled
     * while parked in NetTcp.accept) terminates in state
     * CANCELLED, not DONE. Treat both as "terminated" for the
     * purposes of awaiting. The fiber's `result` is set to
     * kai_unit() on cancel-driven unwind (see the cancel pad
     * setup). */
    if (f->state != KAI_FIBER_DONE && f->state != KAI_FIBER_CANCELLED) {
        /* Park self on f's awaiter chain. The trampoline (in
         * kai_fiber_trampoline) walks this chain on DONE and
         * unparks each awaiter — putting us back on the run queue
         * with state READY. */
        KaiFiber *me = kai_active_fiber;
        me->awaiters_next = f->awaiters_head;
        f->awaiters_head  = me;
        kai_sched_park();
        if (f->state != KAI_FIBER_DONE && f->state != KAI_FIBER_CANCELLED) {
            fprintf(stderr,
                "kai: Spawn.await woken but target not DONE (state=%d)\n",
                (int) f->state);
            exit(1);
        }
    }
    /* For CANCELLED targets `f->result` may be NULL — fall back to
     * unit so the caller's continuation receives a well-typed value
     * regardless of how the target terminated. */
    KaiValue *r = f->result ? kai_incref(f->result) : kai_unit();
    return kai_cont_resume(k, r);
}

static KaiValue *kai_default_spawn_select(void *self, KaiValue *fibs_v, KaiCont *k) {
    (void) self;
    if (!fibs_v || (fibs_v->tag != KAI_CONS && fibs_v->tag != KAI_NIL)) {
        fprintf(stderr, "kai: Spawn.select called on non-list value\n");
        exit(1);
    }
    if (fibs_v->tag == KAI_NIL) {
        fprintf(stderr, "kai: Spawn.select called on empty list\n");
        exit(1);
    }
    /* v1 (Phase 2): walk once for an already-DONE fiber. If none,
     * park on the head. Real race + cancel-losers needs Phase 3
     * (Cancel delivery to losers' yield points) and lands in
     * Phase 4 together with the blocking-mailbox primitives. */
    KaiValue *cur   = fibs_v;
    KaiFiber *head  = NULL;
    while (kai_is_ptr(cur) && cur->tag == KAI_CONS) {
        KaiValue *elem = cur->as.cons.head;
        if (!elem || elem->tag != KAI_FIBER || !elem->as.fib) {
            fprintf(stderr, "kai: Spawn.select: list element is not a fiber\n");
            exit(1);
        }
        KaiFiber *fib = elem->as.fib;
        if (!head) head = fib;
        if (fib->state == KAI_FIBER_DONE) {
            return kai_cont_resume(k, kai_incref(fib->result));
        }
        cur = cur->as.cons.tail;
    }
    /* No fiber DONE on entry. Park on the head; when woken, head
     * must be DONE. */
    KaiFiber *me = kai_active_fiber;
    me->awaiters_next = head->awaiters_head;
    head->awaiters_head = me;
    kai_sched_park();
    if (head->state != KAI_FIBER_DONE) {
        fprintf(stderr,
            "kai: Spawn.select woken but head fiber not DONE (state=%d)\n",
            (int) head->state);
        exit(1);
    }
    return kai_cont_resume(k, kai_incref(head->result));
}

/* Issue #679: detach `target` from whatever reactor waiter list it
 * sits on (if any). Returns 1 if the fiber was found and removed
 * from some list, 0 if it was already runnable / DONE / not parked
 * on the reactor. The caller is responsible for calling
 * `kai_sched_unpark(target)` afterwards so the scheduler reschedules
 * it; we keep detach and unpark separate so other call sites
 * (timer expiry, socket-ready drain) that already manage their own
 * resume sequencing can reuse the detach without double-unparking.
 *
 * The walk is O(N) per list; the lists are short in practice
 * (per-fiber-arena means typically one waiter per fd). All seven
 * lists are checked because a fiber lives on exactly one — the
 * waiter discipline never enqueues the same fiber twice. */
static int kai_reactor_detach_fiber(KaiFiber *target) {
    if (!target) return 0;
    KaiFiber **heads[] = {
        &kai_reactor_socket_read_waiters,
        &kai_reactor_socket_write_waiters,
        &kai_reactor_pid_waiters,
        &kai_reactor_filepool_waiters,
        &kai_reactor_timer_head,
        &kai_reactor_stdin_waiter,
        &kai_reactor_signal_waiter,
    };
    const int n_heads = (int) (sizeof(heads) / sizeof(heads[0]));
    for (int h = 0; h < n_heads; ++h) {
        KaiFiber **link = heads[h];
        while (*link) {
            if (*link == target) {
                *link = target->reactor_next;
                target->reactor_next = NULL;
                target->reactor_wait_pid = 0;
                kai_reactor_parked_count--;
                return 1;
            }
            link = &(*link)->reactor_next;
        }
    }
    return 0;
}

static KaiValue *kai_default_spawn_cancel(void *self, KaiValue *fib_v, KaiCont *k) {
    (void) self;
    if (fib_v && fib_v->tag == KAI_FIBER && fib_v->as.fib) {
        KaiFiber *target = fib_v->as.fib;
        target->cancel_requested = 1;
        /* Issue #679: if the target is parked on a reactor waiter
         * list (e.g. NetTcp.accept blocked on the listener fd, recv
         * blocked on read-ready, timer waiting for a deadline), the
         * flag alone is not enough — the reactor would never wake
         * the fiber until external activity arrived, and the cancel
         * delivery point is the next op-call boundary, which the
         * parked fiber never reaches. Detach the target from its
         * waiter list and unpark it. On resume the syscall retry
         * loop at the park site (or the next op-call lookup
         * prologue) observes `cancel_requested` and unwinds via
         * the Cancel discipline.
         *
         * Detach is a no-op when the target is already runnable /
         * DONE / parked outside the reactor — the cancel flag still
         * lands and is observed at the next op boundary the
         * existing pre-#679 path. */
        if (kai_reactor_detach_fiber(target)) {
            kai_sched_unpark(target);
        }
        /* Delivery (inject Cancel.raise() at the target's next
         * op-call boundary) lands in Phase 3 via the lookup-prologue
         * hook in kai_evidence_lookup_node. The flag-only behaviour
         * is the contract Spawn.cancel commits to; only the *visible
         * effect* changes when Phase 3 lands. */
    }
    return kai_cont_resume(k, kai_unit());
}

/* Tier 2 trap-exit: set the current fiber's trap_exit flag from a
 * Bool argument. With trap_exit=1, a linked peer's termination
 * delivers a "Normal"/"Crashed" string to the fiber's mailbox
 * instead of setting cancel_requested. The fiber must already
 * have a mailbox (typically via with_mailbox) for the delivery to
 * land; without one, the propagation falls back to cancel_requested.
 * Spec: docs/actors.md §*Trap-exit semantics*. */
static KaiValue *kai_default_spawn_set_trap_exit(void *self, KaiValue *on, KaiCont *k) {
    (void) self;
    KaiFiber *f = kai_current_fiber();
    int v = (on && on->tag == KAI_BOOL && on->as.b) ? 1 : 0;
    f->trap_exit = v;
    return kai_cont_resume(k, kai_unit());
}

/* Issue #959 — open a structured-concurrency scope on the current
 * fiber. Subsequent `Spawn.spawn` calls register their child on this
 * scope's children list; `nursery_exit` joins them all. Scopes nest:
 * the new scope's `parent` is the fiber's previous `nursery_top`. */
static KaiValue *kai_default_spawn_scope_enter(void *self, KaiCont *k) {
    (void) self;
    KaiFiber *f = kai_current_fiber();
    KaiNursery *n = (KaiNursery *) calloc(1, sizeof(KaiNursery));
    if (!n) { fprintf(stderr, "kai: out of memory (nursery scope)\n"); exit(1); }
    n->children_head = NULL;
    n->parent        = f->nursery_top;
    f->nursery_top   = n;
    return kai_cont_resume(k, kai_unit());
}

/* Park the current fiber on `child`'s awaiter chain until it reaches
 * DONE or CANCELLED. Mirrors the await park; no result is read — the
 * scope joins for completion, not for the value. */
static void kai_nursery_join_child(KaiFiber *child) {
    if (child->state == KAI_FIBER_DONE || child->state == KAI_FIBER_CANCELLED) {
        return;
    }
    KaiFiber *me = kai_active_fiber;
    me->awaiters_next  = child->awaiters_head;
    child->awaiters_head = me;
    kai_sched_park();
}

/* Cancel every not-yet-terminated child still on the scope list: set
 * the flag, and if the child is parked on the reactor, detach + unpark
 * so the cancel is delivered at its next op boundary. Idempotent. */
static void kai_nursery_cancel_siblings(KaiFiber *head) {
    for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
        if (c->state == KAI_FIBER_DONE || c->state == KAI_FIBER_CANCELLED) {
            continue;
        }
        c->cancel_requested = 1;
        if (kai_reactor_detach_fiber(c)) {
            kai_sched_unpark(c);
        }
    }
}

/* Issue #959 — close the innermost scope: join every child, then on
 * the first child that terminated CANCELLED cancel the remaining
 * siblings, finish the drain (waiting their unwind), release the
 * scope's refs, and re-raise via the current fiber's cancel_pad so
 * the failure propagates out of the nursery body. On a clean drain
 * just release the refs and return unit. */
static KaiValue *kai_default_spawn_scope_exit(void *self, KaiCont *k) {
    (void) self;
    KaiFiber   *f     = kai_current_fiber();
    KaiNursery *scope = f->nursery_top;
    if (!scope) {
        /* No open scope — `nursery_exit` without a matching enter is a
         * runtime invariant violation, but degrade to a no-op rather
         * than crash. */
        return kai_cont_resume(k, kai_unit());
    }
    /* Pop the scope first so children joined below (which run on the
     * scheduler) see the enclosing scope, not this closing one. */
    f->nursery_top = scope->parent;

    KaiFiber *head   = scope->children_head;
    int       failed = 0;
    for (KaiFiber *c = head; c; c = c->scope_sibling_next) {
        kai_nursery_join_child(c);
        /* A child counts as a *failure* only if it terminated
         * CANCELLED without anyone requesting its cancellation — i.e.
         * it raised `Cancel` on its own. A child cancelled on request
         * (`Spawn.cancel` from a sibling, or the cancel_siblings walk
         * below) terminates CANCELLED with `cancel_requested` set and
         * is an expected, non-propagating outcome. */
        if (!failed && c->state == KAI_FIBER_CANCELLED && !c->cancel_requested) {
            failed = 1;
            kai_nursery_cancel_siblings(head);
        }
    }
    /* Release the scope's refs and clear the intrusive links. The
     * children are all terminated now; their wrappers may still be
     * held by the user's `Fiber[T]` handles, so decref (not free). */
    KaiFiber *c = head;
    while (c) {
        KaiFiber *nx = c->scope_sibling_next;
        c->scope_sibling_next = NULL;
        if (c->value) kai_decref(c->value);
        c = nx;
    }
    free(scope);

    if (failed) {
        if (f->cancel_pad_set) {
            f->cancel_delivered = 1;
            longjmp(f->cancel_pad, 1);
            /* Unreachable. */
        }
        /* The root fiber (main) has no cancel_pad — a child crash at
         * the program root is a terminal failure. Match the unhandled
         * Cancel.raise() root behaviour: banner + exit. */
        fputs("kai: nursery child cancelled; no survivors\n", stderr);
        exit(1);
    }
    return kai_cont_resume(k, kai_unit());
}

/* m8 #4 + Phase 3: default Cancel.raise handler. Doc B §`Cancel`/
 * Default handler: an unhandled Cancel.raise() unwinds the fiber
 * cleanly (the runtime delivers "no silent survivors"). For a
 * spawned fiber whose trampoline cancel_pad is live, longjmp to it
 * — same path as the yield-point hook, marks state CANCELLED and
 * resumes the scheduler. For main_fiber (no pad — main never runs
 * through the trampoline), fall back to the m8 v1 behaviour: banner
 * + exit(0). Exit code 0 because cancellation at the program root
 * is an expected termination, not a programmer error. */
static KaiValue *kai_default_cancel_raise(void *self, KaiCont *k) {
    (void) self;
    (void) k;
    KaiFiber *f = kai_current_fiber();
    if (f->cancel_pad_set) {
        f->cancel_delivered = 1;
        longjmp(f->cancel_pad, 1);
        /* Unreachable. */
    }
    fputs("kai: Cancel.raise: unhandled (fiber cancelled)\n", stderr);
    exit(0);
}

/* Phase 5 — Link runtime registry.
 *
 * `Link.link(peer)` registers a bidirectional link between the
 * current fiber and the fiber that owns `peer`'s mailbox. On
 * either fiber's termination, the trampoline walks the linked
 * chain and sets cancel_requested on each peer (delivered at
 * that peer's next yield-point hook).
 *
 * v1 simplifications:
 *  - `Pid[Nothing]` is the type-erased existential pid the typer
 *    uses for link/monitor ops; we resolve via `peer->as.mb->owner_fiber`.
 *  - Self-links (a == b) are dropped.
 *  - If the peer mailbox has no owner_fiber (mailbox alloc'd
 *    outside any fiber context, e.g. before runtime init), the
 *    link is silently dropped — caller cannot observe failure
 *    because the op is `Unit`.
 *  - Duplicate links between the same pair are not de-dup'd; the
 *    propagation walk sets cancel_requested idempotently, so the
 *    duplicates are harmless beyond the wasted KaiLinkNode.
 *  - Spec specifies "crash → propagate cancel"; v1 propagates on
 *    both DONE and CANCELLED termination (BEAM-style trap-exit
 *    semantics queued for post-MVP). */
static void kai_link_add_bidirectional(KaiFiber *a, KaiFiber *b) {
    if (!a || !b || a == b) return;
    KaiLinkNode *na = (KaiLinkNode *) calloc(1, sizeof(KaiLinkNode));
    KaiLinkNode *nb = (KaiLinkNode *) calloc(1, sizeof(KaiLinkNode));
    if (!na || !nb) {
        fprintf(stderr, "kai: out of memory (link)\n");
        exit(1);
    }
    na->peer = b; na->next = a->linked_head; a->linked_head = na;
    nb->peer = a; nb->next = b->linked_head; b->linked_head = nb;
}

/* Walk a fiber's linked chain at termination. For each peer:
 *   - if peer has trap_exit=1 AND a current mailbox, push a
 *     "Normal" or "Crashed" KAI_STR (per `reason`) into the
 *     mailbox. The peer learns of the termination in user code
 *     and is NOT cancelled.
 *   - otherwise, set cancel_requested (the v1 default — delivered
 *     at the peer's next yield-point hook).
 * In either branch, remove our back-link from the peer's chain so
 * the peer's own future termination doesn't re-enter our (now-
 * freed) chain. Each KaiLinkNode is freed as the walk passes it. */
static void kai_link_propagate_terminate(KaiFiber *self, KaiExitReason reason) {
    KaiLinkNode *ln = self->linked_head;
    self->linked_head = NULL;
    while (ln) {
        KaiLinkNode *next = ln->next;
        KaiFiber *peer = ln->peer;
        if (peer) {
            if (peer->trap_exit && peer->mailbox) {
                /* Trap-exit delivery: push the reason string into the
                 * peer's mailbox. The mailbox holds an owning ref on
                 * each msg (kai_mailbox_push convention via
                 * mailbox_send's incref). Wake any parked receiver
                 * — that's exactly what kai_mailbox_push already does
                 * via its recv_waiter handoff. */
                const char *txt = (reason == KAI_EXIT_NORMAL) ? "Normal" : "Crashed";
                kai_mailbox_push(peer->mailbox, kai_str(txt));
            } else {
                peer->cancel_requested = 1;
            }
            /* Remove our back-link from peer's chain (linear scan;
             * v1 chains are short — typically 1-2 entries). */
            KaiLinkNode **slot = &peer->linked_head;
            while (*slot) {
                if ((*slot)->peer == self) {
                    KaiLinkNode *rm = *slot;
                    *slot = rm->next;
                    free(rm);
                    break;
                }
                slot = &(*slot)->next;
            }
        }
        free(ln);
        ln = next;
    }
}

static KaiValue *kai_default_link_link(void *self, KaiValue *peer, KaiCont *k) {
    (void) self;
    if (peer && peer->tag == KAI_PID && peer->as.mb && peer->as.mb->owner_fiber) {
        kai_link_add_bidirectional(kai_current_fiber(), peer->as.mb->owner_fiber);
    }
    return kai_cont_resume(k, kai_unit());
}

/* Issue #103 — return 1 if `f` is bidirectionally linked to any
 * peer with `trap_exit=true`. Used by the Cancel-lookup hook below
 * to decide whether `Cancel.raise()` must bypass user handlers and
 * unwind the fiber directly so the trampoline tail can convert
 * termination into a `"Crashed"` mailbox push on the trap-exit'd
 * peer. The walk is short (link chains are typically 1-2 entries
 * per the v1 simplifications above) so the per-op overhead of this
 * check is negligible. */
static int kai_fiber_has_trap_exit_link(KaiFiber *f) {
    if (!f) return 0;
    KaiLinkNode *ln = f->linked_head;
    while (ln) {
        if (ln->peer && ln->peer->trap_exit) return 1;
        ln = ln->next;
    }
    return 0;
}

/* Issue #103 — bypass user-installed Cancel handlers when the
 * current fiber is linked to a trap-exit'd peer. The contract
 * documented in docs/actors.md §*Trap-exit semantics* requires
 * that a supervisor with `fiber_set_trap_exit(true)` observes a
 * linked child's termination through its mailbox, not through a
 * Cancel handler in the call chain between the spawn point and
 * the receive point. Without this hook, the child's `Cancel.raise()`
 * walks the inherited evidence chain (cloned at spawn time per
 * issue #104) and lands in any outer `with Cancel { raise(_) -> ... }`
 * the parent installed before the spawn — short-circuiting the
 * trampoline-tail link propagation that would have pushed
 * `"Crashed"` into the supervisor's mailbox.
 *
 * The check fires only when the cancel_pad is live (so we have
 * somewhere to longjmp) and the fiber actually has a trap-exit'd
 * link (so this is the linkage-relevant case the issue scopes the
 * fix to). Plain Cancel.raise() inside a fiber that holds no
 * trap-exit'd link still dispatches through user handlers as before,
 * preserving the cleanup-on-cancel idiom for non-supervised work. */
static void kai_check_trap_exit_cancel_bypass(void) {
    KaiFiber *f = kai_active_fiber;
    if (!f || !f->cancel_pad_set) return;
    if (!kai_fiber_has_trap_exit_link(f)) return;
    f->cancel_delivered = 1;
    longjmp(f->cancel_pad, 1);
    /* Unreachable. */
}

/* Tier 2 Monitor — append (observer, target_pid) onto target's
 * monitor_head chain. The runtime takes one ref on target_pid via
 * kai_incref so the value can be pushed into the observer's mailbox
 * at termination time even if the user dropped their last
 * reference. v1 does not deduplicate: monitoring the same pid twice
 * produces two MonitorDown deliveries, matching the BEAM spec. */
static void kai_monitor_add(KaiFiber *target, KaiFiber *observer, KaiValue *target_pid) {
    KaiMonitorNode *n = (KaiMonitorNode *) malloc(sizeof(KaiMonitorNode));
    if (!n) {
        fprintf(stderr, "kai: out of memory (monitor)\n");
        exit(1);
    }
    n->observer   = observer;
    n->target_pid = target_pid;
    if (target_pid) kai_incref(target_pid);
    n->next       = target->monitor_head;
    target->monitor_head = n;
}

/* Tier 2 Monitor — remove the *first* entry on target's chain that
 * matches (observer, target_pid). Demonitor v1 takes the same pid
 * value the user originally passed to monitor(...); equality is
 * by KAI_PID mailbox-pointer identity (kai_op_eq_v's KAI_PID branch).
 * Returns 1 on removal, 0 if no match. */
static int kai_monitor_remove(KaiFiber *target, KaiFiber *observer, KaiValue *target_pid) {
    KaiMonitorNode **slot = &target->monitor_head;
    while (*slot) {
        KaiMonitorNode *cur = *slot;
        int pid_match = 1;
        if (target_pid && cur->target_pid) {
            /* both present — match on mailbox pointer. */
            pid_match =
                cur->target_pid->tag == KAI_PID &&
                target_pid->tag == KAI_PID &&
                cur->target_pid->as.mb == target_pid->as.mb;
        }
        if (cur->observer == observer && pid_match) {
            *slot = cur->next;
            if (cur->target_pid) kai_decref(cur->target_pid);
            free(cur);
            return 1;
        }
        slot = &cur->next;
    }
    return 0;
}

/* Tier 2 Monitor — walk the target fiber's monitor chain at
 * termination. For each (observer, target_pid) entry, push
 * target_pid into observer->mailbox and free the node.
 * Observers without a current mailbox silently drop the
 * delivery (matching trap-exit's "no-mailbox falls back" shape;
 * monitors do not propagate faults so there is no cancel-side
 * fallback to take). The cause distinction (Normal / Crashed) is
 * not encoded in the v1 message — observers that need it can pair
 * Monitor with Link+trap_exit, which delivers the
 * "Normal"/"Crashed" string into the same mailbox. */
static void kai_monitor_propagate_terminate(KaiFiber *self) {
    KaiMonitorNode *mn = self->monitor_head;
    self->monitor_head = NULL;
    while (mn) {
        KaiMonitorNode *next = mn->next;
        KaiFiber *observer = mn->observer;
        if (observer && observer->mailbox && mn->target_pid) {
            /* kai_mailbox_push takes ownership of the incref we
             * stamped at kai_monitor_add time — we hand the ref
             * to the mailbox and don't decref locally. */
            kai_mailbox_push(observer->mailbox, mn->target_pid);
        } else if (mn->target_pid) {
            /* No mailbox to deliver into — drop our owning ref so
             * the pid value can be reclaimed. */
            kai_decref(mn->target_pid);
        }
        free(mn);
        mn = next;
    }
}

static KaiValue *kai_default_monitor_monitor(void *self, KaiValue *target, KaiCont *k) {
    (void) self;
    if (target && target->tag == KAI_PID && target->as.mb && target->as.mb->owner_fiber) {
        kai_monitor_add(target->as.mb->owner_fiber, kai_current_fiber(), target);
    }
    /* v1 simplification — return the same Pid as the ref. The
     * spec's `MonitorRef` is opaque; identifying the monitored
     * fiber by its own pid is sufficient for demonitor and for the
     * fixture-level "which fiber died" pattern. The user-facing
     * type alias `MonitorRef = Pid[Nothing]` is pinned in
     * docs/actors.md §*Monitors — unidirectional* (v1 simplification).
     */
    return kai_cont_resume(k, target);
}

static KaiValue *kai_default_monitor_demonitor(void *self, KaiValue *ref, KaiCont *k) {
    (void) self;
    if (ref && ref->tag == KAI_PID && ref->as.mb && ref->as.mb->owner_fiber) {
        kai_monitor_remove(ref->as.mb->owner_fiber, kai_current_fiber(), ref);
    }
    return kai_cont_resume(k, kai_unit());
}

/* (typedef forward-declared above, before KaiFiber.) */
struct KaiEvidence {
    KaiEvidence *parent;
    const char  *eff_label;     /* canonical effect name (literal or interned). */
    void        *handler;       /* *Ev<Eff> struct; opaque to the runtime. */
    /* m7a #6e: handle's setjmp target + slot to deposit the discarded
     * clause's return value. Set by kai_evidence_push_with_jmp; the
     * op-call site reads them via kai_evidence_lookup_node when
     * status stays UNRESUMED after the clause returns, then longjmps
     * with the clause's value as the handle's body result. NULL when
     * the handle didn't allocate a jmp_buf (the m7a #7 default
     * handlers always resume, so they never longjmp). */
    jmp_buf     *handle_jmp;
    KaiValue   **discard_slot;
};

/* Push an Evidence node onto the current fiber's stack. The caller
 * owns the node's storage — typically `alloca`'d inside a compiled
 * `handle` prologue. This primitive only fills its fields and
 * links it as the new top. */
static void kai_evidence_push(KaiEvidence *node, const char *eff_label, void *handler) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = NULL;
    node->discard_slot = NULL;
    f->evidence_top    = node;
}
/* Connect a frame-supplied default node to its Ev WITHOUT pushing it on the
 * evidence stack: the call site addresses this node directly through the frame,
 * so it needs `handler` / `eff_label` set and the discard fields NULL, but no
 * `parent` / `evidence_top` linkage (a default never discards, never unwinds). */
static void kai_evidence_init_default(KaiEvidence *node, const char *eff_label, void *handler) {
    node->parent       = NULL;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = NULL;
    node->discard_slot = NULL;
}

/* m7a #6e: like kai_evidence_push but also stamps the handle's
 * setjmp/longjmp landing pad. The op-call site uses the node's
 * `handle_jmp` to abandon the body when the clause discards
 * `resume`. Default handlers (m7a #7) always resume, so they call
 * the simpler push variant. */
static void kai_evidence_push_with_jmp(KaiEvidence *node, const char *eff_label,
                                        void *handler, jmp_buf *jmp,
                                        KaiValue **discard_slot) {
    KaiFiber *f = kai_current_fiber();
    node->parent       = f->evidence_top;
    node->eff_label    = eff_label;
    node->handler      = handler;
    node->handle_jmp   = jmp;
    node->discard_slot = discard_slot;
    f->evidence_top    = node;
}

/* Pop the topmost Evidence node. The compiled `handle` epilogue
 * always pairs with a matching push (Doc C §*Per-fiber isolation*
 * balance invariant). Pop on an empty stack is a compiler bug; it
 * silently no-ops here so a malformed prologue/epilogue cannot
 * corrupt unrelated memory. */
static void kai_evidence_pop(void) {
    KaiFiber *f = kai_current_fiber();
    if (f->evidence_top != NULL) {
        f->evidence_top = f->evidence_top->parent;
    }
}

/* Walk the current fiber's stack and return the innermost handler
 * for `eff_label`. Returns NULL if no matching handler is in
 * scope — which would indicate a compiler bug, since the type
 * checker rejects unhandled effects. The fast path is pointer
 * equality (most labels are literal strings shared at the call
 * site); strcmp is the fallback for edge cases like dynamically-
 * generated label strings. */

/* Issue #682 — mirror of the compiler-emitted `struct EvCancel` so
 * the runtime can dispatch `Cancel.raise()` synthetically when a
 * sibling-initiated cancel lands at a yield point with a user
 * `with Cancel { raise(_) -> ... }` handler in scope. The compiler
 * emits the same layout in every translation unit that imports the
 * Cancel effect (see the EvCancel struct in the generated C); the
 * prefix (`handler_id`, `env`, `state`, then op fn pointers) is
 * fixed by the Ev-struct convention documented in
 * `docs/effects-impl.md` §*Evidence layout*. Since Cancel has a
 * single op (`raise`), the runtime mirror is two pointers wide and
 * stable across compiler versions within an edition. */
typedef struct KaiRtEvCancel KaiRtEvCancel;
struct KaiRtEvCancel {
    KaiHandlerId handler_id;
    void        *env;
    KaiValue    *state;
    KaiValue   *(*raise)(KaiRtEvCancel *self, KaiCont *k);
};

/* Phase 3 — Cancel delivery at yield points. Every effect-op call
 * goes through one of the kai_evidence_lookup* functions; we use
 * that as the natural yield-point check.
 *
 * Two paths after the flag is observed:
 *
 *   1. User-installed `handle { ... } with Cancel { raise(_) -> ... }`
 *      is in scope on the current fiber's evidence stack. Dispatch
 *      through the user clause exactly as a direct `Cancel.raise()`
 *      call site would (issue #682 — sibling-initiated cancels must
 *      run the same handler the synchronous path runs, otherwise the
 *      cleanup contract documented in `kai info fibers` is silently
 *      broken). The clause discards `resume` (forced — `raise()`
 *      returns `Nothing`), so we then long-jump to the handle's
 *      landing pad with the discarded value installed in its slot.
 *
 *   2. No user handler in scope. Fall back to the cancel_pad path —
 *      the trampoline's second-return marks the fiber CANCELLED and
 *      continues with the awaiter walk. If the pad is not set
 *      (main_fiber, or outside trampoline scope), the check falls
 *      through and dispatch proceeds normally; a subsequent
 *      user-level `Cancel.raise()` will still hit
 *      kai_default_cancel_raise which exits the program.
 *
 * The user-handler walk explicitly skips `in_dispatch_node` so a
 * Cancel handler that is itself mid-dispatch (a `Cancel.raise()`
 * re-issued from inside its own clause) resolves to an outer Cancel
 * frame instead of recursing into itself — same per-fiber rule
 * `kai_evidence_lookup_node` enforces for user-driven dispatch
 * (m8 bug #12). */
static void kai_check_cancel_yield_point(void) {
    KaiFiber *f = kai_current_fiber();
    if (!(f->cancel_requested && !f->cancel_delivered && f->cancel_pad_set)) {
        return;
    }

    /* Search the evidence stack for the innermost user Cancel handler
     * (one with a live `handle_jmp` — default Cancel handlers do not
     * allocate a jmp_buf because they never longjmp out of their
     * clause). Skip the in-dispatch node (same rule as the by-name
     * lookup) to preserve the recursion-into-outer-frame contract. */
    KaiEvidence *node = f->evidence_top;
    KaiEvidence *user_node = NULL;
    while (node) {
        if (node != f->in_dispatch_node
            && node->handle_jmp != NULL
            && node->eff_label
            && strcmp(node->eff_label, "Cancel") == 0) {
            user_node = node;
            break;
        }
        node = node->parent;
    }

    /* Delivered marker is flipped *before* invoking the clause. The
     * clause body may call into ops that re-enter
     * kai_check_cancel_yield_point — without the early flip those
     * re-entries would see the flag still set and try to dispatch
     * again. */
    f->cancel_delivered = 1;

    if (user_node == NULL) {
        longjmp(f->cancel_pad, 1);
        /* Unreachable. */
    }

    /* Dispatch the user clause. Mirrors the op-call shape emitted by
     * `emit_named_call` (stage2/main.kai §"op call `Eff.op(args)`")
     * for `Cancel.raise()` — bind identity continuation, mark the
     * node as in-dispatch across the call, invoke the clause, and if
     * `resume` was discarded (status stays UNRESUMED — the only
     * legal outcome for raise() because it returns `Nothing`), store
     * the discarded value, pop evidence, and longjmp to the handle's
     * landing pad. */
    KaiRtEvCancel *ev = (KaiRtEvCancel *) user_node->handler;
    KaiCont k;
    kai_cont_init_identity(&k, ev->handler_id);

    KaiEvidence *saved_disp = f->in_dispatch_node;
    f->in_dispatch_node = user_node;
    KaiValue *op_r = ev->raise(ev, &k);
    f->in_dispatch_node = saved_disp;

    if (k.status == KAI_CONT_UNRESUMED && user_node->handle_jmp != NULL) {
        *user_node->discard_slot = op_r;
        kai_evidence_pop();
        longjmp(*user_node->handle_jmp, 1);
        /* Unreachable. */
    }

    /* Defensive: a Cancel clause that calls `resume(_)` is a
     * compiler bug (raise() returns Nothing — there is no Nothing
     * value to feed back). If it ever happens, fall back to the
     * pad so the fiber still terminates cleanly. */
    longjmp(f->cancel_pad, 1);
    /* Unreachable. */
}

static void *kai_evidence_lookup(const char *eff_label) {
    kai_check_cancel_yield_point();
    /* Issue #103 — Cancel-on-linked-trap-exit'd-peer must bypass
     * user handlers (see kai_check_trap_exit_cancel_bypass). */
    if (eff_label && strcmp(eff_label, "Cancel") == 0) {
        kai_check_trap_exit_cancel_bypass();
    }
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        if (node->eff_label == eff_label
            || strcmp(node->eff_label, eff_label) == 0) {
            return node->handler;
        }
        node = node->parent;
    }
    return NULL;
}

/* m7a #6e: same lookup but returns the whole node so the op-call
 * site can reach the handle's jmp_buf if a discard happens. */
static KaiEvidence *kai_evidence_lookup_node(const char *eff_label) {
    kai_check_cancel_yield_point();
    /* Issue #103 — bypass user Cancel handlers when the current
     * fiber is linked to a trap-exit'd peer (see
     * kai_check_trap_exit_cancel_bypass for rationale). */
    if (eff_label && strcmp(eff_label, "Cancel") == 0) {
        kai_check_trap_exit_cancel_bypass();
    }
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        /* m8 bug #12: skip a node whose clause body is currently being
         * dispatched on *this* fiber, so a recursive op resolves to the
         * outer handler. Per-fiber state, not a flag on the node. */
        if (node != f->in_dispatch_node
            && (node->eff_label == eff_label
                || strcmp(node->eff_label, eff_label) == 0)) {
            return node;
        }
        node = node->parent;
    }
    return NULL;
}

/* Resolve a default-bearing effect performed with no caller frame slot: a
 * lexical handler on the stack wins (the walk finds it), else the frame-supplied
 * default node (no longer pushed). The bridge for a direct perform in a fn that
 * carries no frame (e.g. `main`), where the walk alone would miss the default. */
static KaiEvidence *kai_evidence_lookup_or_default(const char *eff_label, KaiEvidence *def) {
    KaiEvidence *node = kai_evidence_lookup_node(eff_label);
    return node != NULL ? node : def;
}

/* A fiber-local effect resolved to no handler — the perform site walked an
 * empty evidence stack. A capability does not cross a spawn, so a fiber-local
 * op (Actor/Cancel/Link/Monitor/Spawn) performed in a fiber whose own body
 * installed no handler for it has no disposition to bind. Report it instead of
 * dereferencing the NULL node. */
static KaiEvidence *kai_evidence_require(KaiEvidence *node, const char *eff_label) {
    /* A NULL node (empty evidence stack) or a node whose handler slot was never
     * filled (a default global minted for a fiber-local effect that has no real
     * default) both mean "no disposition to bind here". Report either, instead
     * of letting a later `node->handler` deref segfault. */
    if (node == NULL || node->handler == NULL) {
        fprintf(stderr, "kai: effect not handled in fiber: %s\n", eff_label);
        exit(1);
    }
    return node;
}

/* A by-id capability (a `var`/State/Reader cell or a `with Eff as a` alias)
 * resolved to no node — the alias's evidence is on the fiber where it was
 * installed and does not cross a spawn. A NULL here means the op ran on a
 * child fiber that does not carry the cell; report it instead of dereferencing
 * the NULL node. The compile-time escape check catches the common shapes; this
 * is the runtime floor for the ones it cannot see statically. */
static KaiEvidence *kai_evidence_require_reachable(KaiEvidence *node, const char *cap_name) {
    if (node == NULL || node->handler == NULL) {
        fprintf(stderr, "kai: capability not reachable in this fiber: %s\n", cap_name);
        exit(1);
    }
    return node;
}

/* m7b #15: per-instance dispatch — find the evidence node whose
 * handler_id matches `id`, no name match required. The codegen
 * uses this when the op call comes from a `with Eff as alias`
 * binding, so an outer alias's op stays reachable even after an
 * inner `with Eff as other` shadows the effect name. */
static KaiEvidence *kai_evidence_lookup_node_by_id(KaiHandlerId id) {
    kai_check_cancel_yield_point();
    KaiFiber *f = kai_current_fiber();
    KaiEvidence *node = f->evidence_top;
    while (node != NULL) {
        /* m8 bug #12: same per-fiber skip rule as the by-name lookup. */
        if (node == f->in_dispatch_node) { node = node->parent; continue; }
        KaiHandlerId nid = ((KaiHandlerId *) node->handler)[0];
        if (nid == id) {
            /* Issue #103 — same Cancel bypass as the by-name path:
             * an aliased `with Cancel as e` op call on a fiber linked
             * to a trap-exit'd peer must unwind directly. */
            if (node->eff_label
                && strcmp(node->eff_label, "Cancel") == 0) {
                kai_check_trap_exit_cancel_bypass();
            }
            return node;
        }
        node = node->parent;
    }
    return NULL;
}

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

/* ===================================================================
 * KIR Lane 1 — in-process libLLVM C-API forwarders (docs/kir-design.md
 * §7.2). OPT-IN ONLY: compiled only under `-DKAI_LLVM` (set by
 * `make KAI_LLVM=1`). The default build and the whole bootstrap chain
 * never see these — no libLLVM header, no libLLVM link.
 *
 * ABI: a forwarder is a plain C function whose handle params/returns
 * are raw `void *` (an LLVM C-API object pointer). On the kaikai side
 * these are typed `TyHandle` (raw, MUnboxed, non-RC); the C path emits
 * a raw UFn call with `void *` args and a `void *` result, so a handle
 * NEVER passes through `kai_int` / the tagged-Int boxing (which would
 * corrupt the pointer). This is the spike that confirms UFn-raw is the
 * right vehicle over the extern-C/FFI shim (which always reboxes the
 * result to `KaiValue *`).
 * =================================================================== */
#ifdef KAI_LLVM
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Linker.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/DebugInfo.h>
#include <stdlib.h>

/* The native backend keeps one context + builder per compilation unit.
 * `kai_llvm_module_new` creates them; the module owns the context for
 * the backend's lifetime (disposed by the process exit, like the
 * out-of-process backends). A handle crosses to kaikai as a raw
 * `void *` (`TyHandle`, never boxed, never RC). */
static LLVMContextRef kai_llvm_ctx = NULL;

/* Create a fresh context + module named `name`. Returns the module
 * handle. `name` is a kaikai `String` (a boxed `KaiValue *`); read its
 * bytes with `->as.s.bytes` (the canonical String payload accessor). */
static void *kai_llvm_module_new(KaiValue *name) {
    kai_llvm_ctx = LLVMContextCreate();
    LLVMModuleRef m = LLVMModuleCreateWithNameInContext(name->as.s.bytes, kai_llvm_ctx);
    if (name) kai_decref(name);
    return (void *) m;
}

/* === Parte B — the native backend context (handles live OUTSIDE the RC
 * regime). ===========================================================
 *
 * The walk threads a lot of LLVM handles (module, builder, the cached
 * types, the current function, and per-function register / basic-block
 * tables). A handle is a raw LLVM pointer, NOT a `KaiValue *`: it carries
 * no refcount. The production kaic2 is compiled by the type-BLIND kaic1,
 * whose `kai_record` / list constructors `kai_incref` every field — which
 * on a handle dereferences an LLVM pointer's non-existent `->rc` and
 * corrupts it (the asu-reviewed "risk #1"). The defence is
 * representational: NO handle ever enters a kaikai record or list. ALL
 * backend state lives in THIS C struct, reached through one opaque
 * `:Handle` the kaikai walk passes around (excluded from Perceus as a
 * `:Handle` param). The register / block tables — `String -> handle` —
 * therefore live here in C (the same off-RC vehicle as the arg buffer),
 * with names `strdup`'d (never a borrowed kaikai pointer kept past the
 * statement) and reset per function (arena-style). The tables store + return
 * handles (stable LLVM pointers), never pointers into the table itself. */
typedef struct { char *name; void *alloca; int slot; } KaiNReg;
typedef struct { char *label; void *bb; } KaiNBlk;
/* Evidence-frame ABI table: per fn symbol, the ordered user-effect slot
 * labels its row demands. Off-RC, strdup'd; resolved by symbol from the
 * signature, call site, and perform. */
typedef struct { char *sym; char **slots; int nslots; } KaiNFrame;
typedef struct {
    void *m, *b, *ptrt, *i64t, *i32t, *voidt, *f64t, *fnval;
    KaiNReg *regs; int nregs, regcap;
    KaiNBlk *blks; int nblks, blkcap;
    KaiNFrame *frames; int nframes, framecap;
    int ok;
    int in_fn;   /* begin_fn/end_fn nesting guard (fail loud, not corrupt) */
    /* DWARF debug info (#500), populated only in --debug. `dib` is the
     * module DIBuilder, `difile` the source DIFile, `dicu` the compile
     * unit, `disub` the CURRENT function's DISubprogram (the scope every
     * `set_loc` attaches to). All NULL in release/default (calloc-zeroed)
     * — `native_di_enabled` gates the emit so a non-debug build is
     * byte-identical to before this lane. */
    void *dib, *difile, *dicu, *disub;
} KaiNativeCtx;

static void *kai_native_ctx_new(void *m) {
    KaiNativeCtx *c = (KaiNativeCtx *) calloc(1, sizeof(KaiNativeCtx));
    c->m = m;
    c->b = LLVMCreateBuilderInContext(LLVMGetModuleContext((LLVMModuleRef) m));
    LLVMContextRef ctx = LLVMGetModuleContext((LLVMModuleRef) m);
    c->ptrt = LLVMPointerTypeInContext(ctx, 0);
    c->i64t = LLVMInt64TypeInContext(ctx);
    c->i32t = LLVMInt32TypeInContext(ctx);
    c->voidt = LLVMVoidTypeInContext(ctx);
    c->f64t = LLVMDoubleTypeInContext(ctx);
    c->fnval = NULL;
    c->regs = NULL; c->nregs = 0; c->regcap = 0;
    c->blks = NULL; c->nblks = 0; c->blkcap = 0;
    c->frames = NULL; c->nframes = 0; c->framecap = 0;
    c->ok = 1; c->in_fn = 0;
    return (void *) c;
}
/* Append `eff` to fn `sym`'s frame slot list, in canonical order. Returns
 * kai_unit() — the prim ABI never returns C void. */
static KaiValue *kai_native_ctx_add_frame_slot(void *cv, KaiValue *symv, KaiValue *effv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    const char *sym = symv->as.s.bytes;
    const char *eff = effv->as.s.bytes;
    KaiNFrame *fr = NULL;
    for (int i = 0; i < c->nframes; i++)
        if (strcmp(c->frames[i].sym, sym) == 0) { fr = &c->frames[i]; break; }
    if (!fr) {
        if (c->nframes == c->framecap) {
            c->framecap = c->framecap ? c->framecap * 2 : 16;
            c->frames = (KaiNFrame *) realloc(c->frames, (size_t) c->framecap * sizeof(KaiNFrame));
        }
        fr = &c->frames[c->nframes++];
        fr->sym = strdup(sym); fr->slots = NULL; fr->nslots = 0;
    }
    fr->slots = (char **) realloc(fr->slots, (size_t) (fr->nslots + 1) * sizeof(char *));
    fr->slots[fr->nslots++] = strdup(eff);
    if (symv) kai_decref(symv);
    if (effv) kai_decref(effv);
    return kai_unit();
}
static int64_t kai_native_ctx_frame_slot_count(void *cv, KaiValue *symv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    const char *sym = symv->as.s.bytes;
    int64_t r = 0;
    for (int i = 0; i < c->nframes; i++)
        if (strcmp(c->frames[i].sym, sym) == 0) { r = (int64_t) c->frames[i].nslots; break; }
    if (symv) kai_decref(symv);
    return r;
}
static KaiValue *kai_native_ctx_frame_slot_eff(void *cv, KaiValue *symv, int64_t j) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    const char *sym = symv->as.s.bytes;
    const char *r = "";
    for (int i = 0; i < c->nframes; i++)
        if (strcmp(c->frames[i].sym, sym) == 0) {
            if (j >= 0 && j < c->frames[i].nslots) r = c->frames[i].slots[j];
            break;
        }
    if (symv) kai_decref(symv);
    return kai_str(r);
}
static int64_t kai_native_ctx_frame_slot_index(void *cv, KaiValue *symv, KaiValue *effv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    const char *sym = symv->as.s.bytes;
    const char *eff = effv->as.s.bytes;
    int64_t r = -1;
    for (int i = 0; i < c->nframes; i++)
        if (strcmp(c->frames[i].sym, sym) == 0) {
            for (int j = 0; j < c->frames[i].nslots; j++)
                if (strcmp(c->frames[i].slots[j], eff) == 0) { r = (int64_t) j; break; }
            break;
        }
    if (symv) kai_decref(symv);
    if (effv) kai_decref(effv);
    return r;
}
static void *kai_native_ctx_b(void *c)     { return ((KaiNativeCtx *) c)->b; }
static void *kai_native_ctx_m(void *c)     { return ((KaiNativeCtx *) c)->m; }
static void *kai_native_ctx_ptrt(void *c)  { return ((KaiNativeCtx *) c)->ptrt; }
static void *kai_native_ctx_i64t(void *c)  { return ((KaiNativeCtx *) c)->i64t; }
static void *kai_native_ctx_i32t(void *c)  { return ((KaiNativeCtx *) c)->i32t; }
static void *kai_native_ctx_voidt(void *c) { return ((KaiNativeCtx *) c)->voidt; }
static void *kai_native_ctx_f64t(void *c) { return ((KaiNativeCtx *) c)->f64t; }
/* An f64 constant from a raw `double` (the caller unboxes the `KRealV`
 * payload — `->as.r` — so the prim takes the scalar directly, matching the
 * unbox-pass / stage-1 `AReal` marshalling. Avoids a boxed-Real param that
 * the C-direct unbox pass would `->as.r` anyway, mismatching the type). */
static void *kai_llvm_const_real(void *f64ty, double d) {
    return (void *) LLVMConstReal((LLVMTypeRef) f64ty, d);
}
static void *kai_native_ctx_fnval(void *c) { return ((KaiNativeCtx *) c)->fnval; }
static KaiValue *kai_native_ctx_set_fnval(void *c, void *fn) { ((KaiNativeCtx *) c)->fnval = fn; return kai_unit(); }
static int64_t kai_native_ctx_ok(void *c)  { return ((KaiNativeCtx *) c)->ok ? 1 : 0; }
static KaiValue *kai_native_ctx_fail(void *c) { ((KaiNativeCtx *) c)->ok = 0; return kai_unit(); }

/* Reset the per-function register + block tables (arena: free the strdup'd
 * names, keep the buffers for reuse). `in_fn` guards against compiling a
 * function inside another (would clobber `fnval`/tables) — fail loud. */
static KaiValue *kai_native_ctx_begin_fn(void *cv, void *fnval) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (c->in_fn) { fprintf(stderr, "kai: native begin_fn nested (compiler bug)\n"); c->ok = 0; return kai_unit(); }
    for (int i = 0; i < c->nregs; i++) free(c->regs[i].name);
    for (int i = 0; i < c->nblks; i++) free(c->blks[i].label);
    c->nregs = 0; c->nblks = 0; c->fnval = fnval; c->in_fn = 1;
    return kai_unit();
}
static KaiValue *kai_native_ctx_end_fn(void *cv) { ((KaiNativeCtx *) cv)->in_fn = 0; return kai_unit(); }

static KaiValue *kai_native_ctx_add_reg(void *cv, KaiValue *name, void *alloca, int64_t slot) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (c->nregs == c->regcap) {
        c->regcap = c->regcap ? c->regcap * 2 : 16;
        c->regs = (KaiNReg *) realloc(c->regs, (size_t) c->regcap * sizeof(KaiNReg));
    }
    c->regs[c->nregs].name = strdup(name->as.s.bytes);
    c->regs[c->nregs].alloca = alloca;
    c->regs[c->nregs].slot = (int) slot;
    c->nregs++;
    if (name) kai_decref(name);
    return kai_unit();
}
/* Returns the alloca handle, or NULL if absent. The slot is read
 * separately (`reg_slot`) so the two stay one source. */
static void *kai_native_ctx_find_reg(void *cv, KaiValue *name) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    void *r = NULL;
    for (int i = 0; i < c->nregs; i++)
        if (strcmp(c->regs[i].name, name->as.s.bytes) == 0) { r = c->regs[i].alloca; break; }
    if (name) kai_decref(name);
    return r;
}
static int64_t kai_native_ctx_reg_slot(void *cv, KaiValue *name) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    int64_t s = -1;
    for (int i = 0; i < c->nregs; i++)
        if (strcmp(c->regs[i].name, name->as.s.bytes) == 0) { s = c->regs[i].slot; break; }
    if (name) kai_decref(name);
    return s;
}
/* The alloca of the i-th register (params are collected first, in order,
 * so `pN` of a tcrec reloop selects the N-th register). NULL if out of
 * range. */
static void *kai_native_ctx_reg_at(void *cv, int64_t i) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    return (i >= 0 && i < c->nregs) ? c->regs[i].alloca : NULL;
}
/* The slot TAG of the i-th register (params collected first, in order),
 * so a `KTcrecGoto` reloop can re-materialise the `pN` value at the param's
 * declared slot — RAW (`i64`/`f64`/`i1`) for a P3 raw param, so the back-
 * edge store matches the alloca type (a boxed store into an `i64` alloca
 * would mis-type, and re-boxing each iteration is the regression P3 fixes).
 * -1 if out of range (a lowering bug). */
static int64_t kai_native_ctx_reg_slot_at(void *cv, int64_t i) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    return (i >= 0 && i < c->nregs) ? c->regs[i].slot : -1;
}
static KaiValue *kai_native_ctx_add_block(void *cv, KaiValue *label, void *bb) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (c->nblks == c->blkcap) {
        c->blkcap = c->blkcap ? c->blkcap * 2 : 16;
        c->blks = (KaiNBlk *) realloc(c->blks, (size_t) c->blkcap * sizeof(KaiNBlk));
    }
    c->blks[c->nblks].label = strdup(label->as.s.bytes);
    c->blks[c->nblks].bb = bb;
    c->nblks++;
    if (label) kai_decref(label);
    return kai_unit();
}
static void *kai_native_ctx_find_block(void *cv, KaiValue *label) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    void *r = NULL;
    for (int i = 0; i < c->nblks; i++)
        if (strcmp(c->blks[i].label, label->as.s.bytes) == 0) { r = c->blks[i].bb; break; }
    if (label) kai_decref(label);
    return r;
}
/* The first block (the loop header a tcrec back-edge branches to). */
static void *kai_native_ctx_first_block(void *cv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    return c->nblks > 0 ? c->blks[0].bb : NULL;
}

/* --- types (in the module's context) --- */
static void *kai_llvm_int64_type(void *m) {
    return (void *) LLVMInt64TypeInContext(LLVMGetModuleContext((LLVMModuleRef) m));
}
static void *kai_llvm_int32_type(void *m) {
    return (void *) LLVMInt32TypeInContext(LLVMGetModuleContext((LLVMModuleRef) m));
}
static void *kai_llvm_ptr_type(void *m) {
    /* Opaque pointer (LLVM ≥ 15): one `ptr` type, address space 0.
     * Models `%KaiValue*` without a pointee — exactly the opaque-ptr
     * world stage2/runtime_llvm.c already relies on. */
    return (void *) LLVMPointerTypeInContext(LLVMGetModuleContext((LLVMModuleRef) m), 0);
}
static void *kai_llvm_void_type(void *m) {
    return (void *) LLVMVoidTypeInContext(LLVMGetModuleContext((LLVMModuleRef) m));
}
/* Float (32-bit) type — the C `float` width an `F32` FFI arg crosses as. */
static void *kai_llvm_float_type(void *m) {
    return (void *) LLVMFloatTypeInContext(LLVMGetModuleContext((LLVMModuleRef) m));
}
/* An integer type of an arbitrary bit width (8/16/32/64) — the exact C
 * width a fixed-width FFI scalar crosses as. */
static void *kai_llvm_int_type(void *m, int64_t bits) {
    return (void *) LLVMIntTypeInContext(LLVMGetModuleContext((LLVMModuleRef) m), (unsigned) bits);
}
static void *kai_llvm_fn_type_0(void *ret) {
    return (void *) LLVMFunctionType((LLVMTypeRef) ret, NULL, 0, 0);
}
static void *kai_llvm_fn_type_1(void *ret, void *p0) {
    LLVMTypeRef params[1]; params[0] = (LLVMTypeRef) p0;
    return (void *) LLVMFunctionType((LLVMTypeRef) ret, params, 1, 0);
}

/* --- functions / blocks / builder --- */
static void *kai_llvm_add_function(void *m, KaiValue *name, void *fnty) {
    LLVMValueRef fn = LLVMAddFunction((LLVMModuleRef) m, name->as.s.bytes, (LLVMTypeRef) fnty);
    if (name) kai_decref(name);
    return (void *) fn;
}
static void *kai_llvm_append_block(void *m, void *fn, KaiValue *name) {
    LLVMBasicBlockRef bb = LLVMAppendBasicBlockInContext(
        LLVMGetModuleContext((LLVMModuleRef) m), (LLVMValueRef) fn, name->as.s.bytes);
    if (name) kai_decref(name);
    return (void *) bb;
}
static void *kai_llvm_builder_new(void *m) {
    return (void *) LLVMCreateBuilderInContext(LLVMGetModuleContext((LLVMModuleRef) m));
}
static KaiValue *kai_llvm_position_at_end(void *b, void *bb) {
    LLVMPositionBuilderAtEnd((LLVMBuilderRef) b, (LLVMBasicBlockRef) bb);
    return kai_unit();
}

/* --- values / instructions --- */
static void *kai_llvm_const_int(void *i64ty, int64_t v) {
    return (void *) LLVMConstInt((LLVMTypeRef) i64ty, (unsigned long long) v, 1);
}
static void *kai_llvm_build_call_0(void *b, void *fn, void *fnty) {
    return (void *) LLVMBuildCall2((LLVMBuilderRef) b, (LLVMTypeRef) fnty,
                                   (LLVMValueRef) fn, NULL, 0, "");
}
static void *kai_llvm_build_call_1(void *b, void *fn, void *fnty, void *a0) {
    LLVMValueRef args[1]; args[0] = (LLVMValueRef) a0;
    return (void *) LLVMBuildCall2((LLVMBuilderRef) b, (LLVMTypeRef) fnty,
                                   (LLVMValueRef) fn, args, 1, "");
}
static KaiValue *kai_llvm_build_ret(void *b, void *v) {
    LLVMBuildRet((LLVMBuilderRef) b, (LLVMValueRef) v);
    return kai_unit();
}
static KaiValue *kai_llvm_build_ret_void(void *b) {
    LLVMBuildRetVoid((LLVMBuilderRef) b);
    return kai_unit();
}

/* === Parte B: the generic KIR walk's C-API surface ===================
 * The spine (above) builds `main -> 42`. The generic walk needs the rest
 * of the IRBuilder surface: N-ary fn types + calls, the alloca/load/store
 * model (every named KIR register is an entry-block alloca, mem2reg
 * promotes it — asu review), the control terminators (br/condbr/switch),
 * constants, globals, and a few raw readers. Handles stay raw `void *`,
 * never boxed, never RC. */

/* --- N-ary function types ---
 * An arg buffer is a heap `void *[]` of LLVM handles the kaikai side
 * fills push-by-push (a `[Handle]` list cannot carry non-RC handles —
 * the list would dup/drop them — so the buffer lives in C, off the RC
 * regime). `cap` grows geometrically; `n` is the live count. */
typedef struct { void **xs; int64_t n; int64_t cap; } KaiLlvmBuf;
static void *kai_llvm_buf_new(void) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) malloc(sizeof(KaiLlvmBuf));
    bf->cap = 8; bf->n = 0;
    bf->xs = (void **) malloc((size_t) bf->cap * sizeof(void *));
    return (void *) bf;
}
static KaiValue *kai_llvm_buf_push(void *buf, void *h) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    if (bf->n == bf->cap) {
        bf->cap *= 2;
        bf->xs = (void **) realloc(bf->xs, (size_t) bf->cap * sizeof(void *));
    }
    bf->xs[bf->n++] = h;
    return kai_unit();
}
static KaiValue *kai_llvm_buf_free(void *buf) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    free(bf->xs); free(bf);
    return kai_unit();
}
/* The live count + the i-th handle of a buffer (the off-RC replacement
 * for `list_length` / indexing on a `[Handle]` list). */
static int64_t kai_llvm_buf_len(void *buf) { return ((KaiLlvmBuf *) buf)->n; }
static void *kai_llvm_buf_get(void *buf, int64_t i) { return ((KaiLlvmBuf *) buf)->xs[i]; }
/* A function type `ret (params...)` whose param-type handles are in `buf`. */
static void *kai_llvm_fn_type_n(void *ret, void *buf) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    return (void *) LLVMFunctionType((LLVMTypeRef) ret,
                                     (LLVMTypeRef *) bf->xs, (unsigned) bf->n, 0);
}
/* A struct type over a buffer of element types (non-packed: natural C
 * padding), for an `extern "C" type` passed by value. */
static void *kai_llvm_struct_type(void *m, void *buf) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    return (void *) LLVMStructTypeInContext(LLVMGetModuleContext((LLVMModuleRef) m),
                                            (LLVMTypeRef *) bf->xs, (unsigned) bf->n, 0);
}
/* GEP to the i-th field of a struct held in memory. The struct is passed
 * to / returned from a C extern by value via a `byval`/`sret` pointer;
 * the shim stores/loads each field through this. */
static void *kai_llvm_build_struct_gep(void *b, void *sty, void *ptr, int64_t i) {
    return (void *) LLVMBuildStructGEP2((LLVMBuilderRef) b, (LLVMTypeRef) sty,
                                        (LLVMValueRef) ptr, (unsigned) i, "");
}
/* `byval(<struct>)` on a parameter — the directive that makes LLVM apply
 * the target C-ABI for struct-by-value (registers vs indirect per
 * SysV/AAPCS), so a clang-compiled callee receives the struct correctly.
 * Applied to BOTH the extern declaration's param and the call site, at the
 * same 1-based param index (index 0 is the return). */
static void kai_llvm_byval_attr_at(LLVMModuleRef m, LLVMValueRef fn_or_call,
                                   int is_call, int64_t param_ix, LLVMTypeRef sty) {
    LLVMContextRef ctx = LLVMGetModuleContext(m);
    unsigned kind = LLVMGetEnumAttributeKindForName("byval", 5);
    LLVMAttributeRef a = LLVMCreateTypeAttribute(ctx, kind, sty);
    LLVMAttributeIndex ix = (LLVMAttributeIndex) (param_ix + 1);
    if (is_call) LLVMAddCallSiteAttribute(fn_or_call, ix, a);
    else LLVMAddAttributeAtIndex(fn_or_call, ix, a);
}
static KaiValue *kai_llvm_add_byval_decl(void *m, void *fn, int64_t param_ix, void *sty) {
    kai_llvm_byval_attr_at((LLVMModuleRef) m, (LLVMValueRef) fn, 0, param_ix, (LLVMTypeRef) sty);
    return kai_unit();
}
static KaiValue *kai_llvm_add_byval_call(void *m, void *call, int64_t param_ix, void *sty) {
    kai_llvm_byval_attr_at((LLVMModuleRef) m, (LLVMValueRef) call, 1, param_ix, (LLVMTypeRef) sty);
    return kai_unit();
}
/* `n` copies of one pointer type, for the all-boxed fn signatures the
 * KIR lowers (every param/return is `ptr`). */
static void *kai_llvm_fn_type_boxed(void *ptr_t, int64_t n) {
    LLVMTypeRef stack[16];
    LLVMTypeRef *ps = stack;
    LLVMTypeRef *heap = NULL;
    if (n > 16) { heap = (LLVMTypeRef *) malloc((size_t) n * sizeof(LLVMTypeRef)); ps = heap; }
    for (int64_t i = 0; i < n; i++) ps[i] = (LLVMTypeRef) ptr_t;
    LLVMTypeRef t = LLVMFunctionType((LLVMTypeRef) ptr_t, ps, (unsigned) n, 0);
    if (heap) free(heap);
    return (void *) t;
}
/* An N-ary call to a known function value (the args are in `buf`). */
static void *kai_llvm_build_call_n(void *b, void *fn, void *fnty, void *buf) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    return (void *) LLVMBuildCall2((LLVMBuilderRef) b, (LLVMTypeRef) fnty,
                                   (LLVMValueRef) fn, (LLVMValueRef *) bf->xs,
                                   (unsigned) bf->n, "");
}

/* --- function lookup / declaration ---
 * Resolve a function by name; declare it (no body) if absent. The walk
 * uses this for runtime externs (`kaix_*`) and forward references to
 * user fns the module defines later. `LLVMGetNamedFunction` returns NULL
 * when absent, so a miss adds the declaration with the given type. */
static void *kai_llvm_get_or_declare_fn(void *m, KaiValue *name, void *fnty) {
    LLVMValueRef fn = LLVMGetNamedFunction((LLVMModuleRef) m, name->as.s.bytes);
    /* LLVMGetNamedFunction consults the module's ValueSymbolTable, which on
     * LLVM 22 does NOT reliably index functions created earlier in this same
     * walk (it returned NULL for a function the GetFirst/GetNext iterator
     * still found in the module — the VST lazily de-syncs after many adds).
     * Fall back to a linear scan (the iterator IS authoritative) so a
     * forward-declared fn is reused, not re-added under a `.N` suffix. */
    if (fn == NULL) {
        for (LLVMValueRef g = LLVMGetFirstFunction((LLVMModuleRef) m); g; g = LLVMGetNextFunction(g)) {
            if (strcmp(LLVMGetValueName(g), name->as.s.bytes) == 0) { fn = g; break; }
        }
    }
    if (fn == NULL) fn = LLVMAddFunction((LLVMModuleRef) m, name->as.s.bytes, (LLVMTypeRef) fnty);
    if (name) kai_decref(name);
    return (void *) fn;
}
/* The i-th parameter value of a function (PDirect param reads). */
static void *kai_llvm_get_param(void *fn, int64_t i) {
    return (void *) LLVMGetParam((LLVMValueRef) fn, (unsigned) i);
}
/* An `[n x elem]` array type — the stack buffer a runtime ctor reads
 * `args[i]` from (a `KaiValue*[]`, so `elem` is `ptr`). */
static void *kai_llvm_array_type(void *elem, int64_t n) {
    return (void *) LLVMArrayType((LLVMTypeRef) elem, (unsigned) n);
}
/* `getelementptr [n x elem], ptr arr, i32 0, i32 idx` — the address of
 * the idx-th element of an alloca'd array. Two indices: the leading 0
 * steps through the array pointer, `idx` selects the element. */
static void *kai_llvm_build_array_gep(void *b, void *arrty, void *arr, void *idx) {
    LLVMValueRef ixs[2];
    /* The leading 0 index must be an i32 in the SAME context as the
     * module — `LLVMInt32Type()` is the global context, which on a
     * private-context module yields a cross-context Value* the verifier
     * crashes on. Derive the context from the array type. */
    LLVMContextRef ctx = LLVMGetTypeContext((LLVMTypeRef) arrty);
    ixs[0] = LLVMConstInt(LLVMInt32TypeInContext(ctx), 0, 0);
    ixs[1] = (LLVMValueRef) idx;
    return (void *) LLVMBuildGEP2((LLVMBuilderRef) b, (LLVMTypeRef) arrty,
                                  (LLVMValueRef) arr, ixs, 2, "");
}
/* `args[i]` of a thunk's `KaiValue** args` param: `getelementptr ptr,
 * ptr args, i64 i` then `load ptr`. One index (a plain pointer, not an
 * array alloca), so a single-index GEP over the element type `ptrt`. */
static void *kai_llvm_build_load_arg(void *b, void *args, void *ptrt, int64_t i) {
    LLVMContextRef ctx = LLVMGetTypeContext((LLVMTypeRef) ptrt);
    LLVMValueRef idx = LLVMConstInt(LLVMInt64TypeInContext(ctx), (unsigned long long) i, 0);
    LLVMValueRef slot = LLVMBuildGEP2((LLVMBuilderRef) b, (LLVMTypeRef) ptrt,
                                      (LLVMValueRef) args, &idx, 1, "");
    return (void *) LLVMBuildLoad2((LLVMBuilderRef) b, (LLVMTypeRef) ptrt, slot, "");
}

/* --- the alloca / load / store model (asu review: every named register
 * is an entry-block alloca; mem2reg promotes it to SSA+phi). --- */
static void *kai_llvm_build_alloca(void *b, void *ty, KaiValue *name) {
    LLVMValueRef a = LLVMBuildAlloca((LLVMBuilderRef) b, (LLVMTypeRef) ty, name->as.s.bytes);
    if (name) kai_decref(name);
    return (void *) a;
}
/* Build a fixed-size alloca in the CURRENT function's entry block, then
 * restore the builder to where it was. A call-site arg buffer (`kaix_apply`
 * / `kaix_variant` / `kaix_record`) must NOT alloca in a loop body: a
 * `tcrec`/TRMC goto-loop re-executes that block per iteration, so an alloca
 * there grows the stack each iteration (fixed size, but N times) and a fiber
 * (64 KiB stack) overflows after ~8 K iterations — issue #668's `list.map`
 * inside a fiber. The C-direct oracle uses a frame-scoped `KaiValue *args[n]`
 * (allocated once); this hoists the equivalent alloca to the entry block so
 * the goto-loop reuses one slot, exactly the "every alloca is an entry-block
 * alloca" invariant the named registers already follow. Inserts before the
 * entry block's terminator (the `br` to the loop header is already built when
 * a body block emits a call), so the module stays well-formed. */
static void *kai_llvm_build_alloca_entry(void *cv, void *ty, KaiValue *name) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    LLVMBuilderRef b = (LLVMBuilderRef) c->b;
    LLVMBasicBlockRef cur = LLVMGetInsertBlock(b);
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock((LLVMValueRef) c->fnval);
    LLVMValueRef first = LLVMGetFirstInstruction(entry);
    if (first) LLVMPositionBuilderBefore(b, first);
    else LLVMPositionBuilderAtEnd(b, entry);
    LLVMValueRef a = LLVMBuildAlloca(b, (LLVMTypeRef) ty, name->as.s.bytes);
    LLVMPositionBuilderAtEnd(b, cur);
    if (name) kai_decref(name);
    return (void *) a;
}
/* `alloca elem, count` — a runtime-sized stack buffer (the handle frame's
 * `jmp_buf`, whose `sizeof` is a runtime value via `kai_jmpbuf_size` rather
 * than a platform-specific N baked into the IR). `count` is an i64 Value. */
static void *kai_llvm_build_array_alloca(void *b, void *elemty, void *count, KaiValue *name) {
    LLVMValueRef a = LLVMBuildArrayAlloca((LLVMBuilderRef) b, (LLVMTypeRef) elemty,
                                          (LLVMValueRef) count, name->as.s.bytes);
    if (name) kai_decref(name);
    return (void *) a;
}
static KaiValue *kai_llvm_build_store(void *b, void *val, void *ptr) {
    LLVMBuildStore((LLVMBuilderRef) b, (LLVMValueRef) val, (LLVMValueRef) ptr);
    return kai_unit();
}
static void *kai_llvm_build_load(void *b, void *ty, void *ptr) {
    return (void *) LLVMBuildLoad2((LLVMBuilderRef) b, (LLVMTypeRef) ty, (LLVMValueRef) ptr, "");
}
/* The module global named `name` (its address as a Value*), or NULL when no
 * such global exists. The call site addresses a default node minted in
 * `kai_main_install_defaults` by this name. */
static void *kai_llvm_get_named_global(void *m, KaiValue *name) {
    LLVMValueRef g = LLVMGetNamedGlobal((LLVMModuleRef) m, name->as.s.bytes);
    if (name) kai_decref(name);
    return (void *) g;
}
/* True when a handle (an LLVM Value pointer) is the null pointer: the
 * discriminant for `get_named_global` returning "no such global". */
static int32_t kai_llvm_handle_is_null(void *h) { return h == NULL ? 1 : 0; }
/* Position the builder at the START of a block (allocas must precede the
 * block's other instructions to stay promotable + static). */
static KaiValue *kai_llvm_position_at_start(void *b, void *bb) {
    LLVMValueRef first = LLVMGetFirstInstruction((LLVMBasicBlockRef) bb);
    if (first) LLVMPositionBuilderBefore((LLVMBuilderRef) b, first);
    else LLVMPositionBuilderAtEnd((LLVMBuilderRef) b, (LLVMBasicBlockRef) bb);
    return kai_unit();
}

/* --- control terminators --- */
static KaiValue *kai_llvm_build_br(void *b, void *bb) {
    LLVMBuildBr((LLVMBuilderRef) b, (LLVMBasicBlockRef) bb);
    return kai_unit();
}
static KaiValue *kai_llvm_build_cond_br(void *b, void *cond, void *then_bb, void *else_bb) {
    LLVMBuildCondBr((LLVMBuilderRef) b, (LLVMValueRef) cond,
                    (LLVMBasicBlockRef) then_bb, (LLVMBasicBlockRef) else_bb);
    return kai_unit();
}
static void *kai_llvm_build_switch(void *b, void *val, void *default_bb, int64_t ncases) {
    return (void *) LLVMBuildSwitch((LLVMBuilderRef) b, (LLVMValueRef) val,
                                    (LLVMBasicBlockRef) default_bb, (unsigned) ncases);
}
static KaiValue *kai_llvm_add_case(void *sw, void *onval, void *bb) {
    LLVMAddCase((LLVMValueRef) sw, (LLVMValueRef) onval, (LLVMBasicBlockRef) bb);
    return kai_unit();
}
static KaiValue *kai_llvm_build_unreachable(void *b) {
    LLVMBuildUnreachable((LLVMBuilderRef) b);
    return kai_unit();
}
/* `icmp ne i32 %v, 0` → i1 — turn a `kaix_truthy` i32 result (0/1) into
 * the i1 a `condbr` needs. */
static void *kai_llvm_build_icmp_ne_zero(void *b, void *v, void *i32ty) {
    LLVMValueRef zero = LLVMConstInt((LLVMTypeRef) i32ty, 0, 0);
    return (void *) LLVMBuildICmp((LLVMBuilderRef) b, LLVMIntNE, (LLVMValueRef) v, zero, "");
}
/* Raw `double` arithmetic for the unboxed-Real path (KIR mode-slave to the
 * unbox pass). `op` selects the operator: 0=`fadd`, 1=`fsub`, 2=`fmul`,
 * 3=`fdiv` — matching the C-direct oracle's `kair_a + kair_b` on a raw
 * `double`. Operands + result are `f64` LLVM values, NOT boxed Reals: no
 * RC, no `kaix_mul` consume, so the use-after-free the boxed path hit on a
 * multi-use raw operand cannot arise. */
static void *kai_llvm_build_fbinop(void *b, int64_t op, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    switch (op) {
        case 0:  return (void *) LLVMBuildFAdd(bld, la, lc, "");
        case 1:  return (void *) LLVMBuildFSub(bld, la, lc, "");
        case 2:  return (void *) LLVMBuildFMul(bld, la, lc, "");
        default: return (void *) LLVMBuildFDiv(bld, la, lc, "");
    }
}
/* Raw `double` ordered comparison → an `i1`. `pred` selects the predicate:
 * 0=`<`, 1=`>`, 2=`<=`, 3=`>=`, 4=`==`, 5=`!=` — the ordered (`O*`) family,
 * matching the C `<`/`==` on a raw `double` (NaN compares false, as C does).
 * The result is the `i1` a `condbr` consumes directly, or the caller boxes
 * via `kaix_bool` at a raw→boxed border. */
static void *kai_llvm_build_fcmp(void *b, int64_t pred, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    LLVMRealPredicate p;
    switch (pred) {
        case 0:  p = LLVMRealOLT; break;
        case 1:  p = LLVMRealOGT; break;
        case 2:  p = LLVMRealOLE; break;
        case 3:  p = LLVMRealOGE; break;
        case 4:  p = LLVMRealOEQ; break;
        default: p = LLVMRealONE; break;
    }
    return (void *) LLVMBuildFCmp(bld, p, la, lc, "");
}
/* Negate a raw `double` (`-x`) — the unary-minus raw form (`(-kair_x)`). */
static void *kai_llvm_build_fneg(void *b, void *a) {
    return (void *) LLVMBuildFNeg((LLVMBuilderRef) b, (LLVMValueRef) a, "");
}
/* Raw `i1` short-circuit boolean (`a && c` / `a || c`) for the unboxed-Bool
 * path. `op`: 1=and, 0=or. Both operands are already evaluated (the unbox
 * pass only marks the node raw when both children are raw), so a STRICT
 * bitwise `and`/`or` on two `i1` values reproduces the C `&&`/`||` result —
 * the oracle's raw `int` logical. */
static void *kai_llvm_build_logical(void *b, int64_t op, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    return op ? (void *) LLVMBuildAnd(bld, la, lc, "")
              : (void *) LLVMBuildOr(bld, la, lc, "");
}
/* Logical NOT of a raw `i1` (`!x`). */
static void *kai_llvm_build_lnot(void *b, void *a) {
    return (void *) LLVMBuildNot((LLVMBuilderRef) b, (LLVMValueRef) a, "");
}
/* Widen an `i1` (an `fcmp`/`icmp` result) to the `i32` a `kaix_bool` box
 * takes (its param is `i32` 0/1). Mirror of how the C-direct oracle's raw
 * bool (`0`/`1`) feeds `kai_bool`. */
static void *kai_llvm_build_zext_i1_i32(void *b, void *v, void *i32ty) {
    return (void *) LLVMBuildZExt((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) i32ty, "");
}
/* Integer narrow/widen for fixed-width FFI marshalling. `trunc` narrows
 * i64→iN at the call (C-cast, no range-check); `sext`/`zext` widen iN→i64
 * on return — `sext` for a signed `I*`, `zext` for an unsigned `U*`
 * (picking the wrong one turns `uint8_t 255` into Int -1). */
static void *kai_llvm_build_trunc(void *b, void *v, void *ty) {
    return (void *) LLVMBuildTrunc((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
}
static void *kai_llvm_build_sext(void *b, void *v, void *ty) {
    return (void *) LLVMBuildSExt((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
}
static void *kai_llvm_build_zext(void *b, void *v, void *ty) {
    return (void *) LLVMBuildZExt((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
}
/* Float narrow/widen: `double`→`float` at an F32 call, `float`→`double`
 * on return. One `fpcast` covers both directions. */
static void *kai_llvm_build_fpcast(void *b, void *v, void *ty) {
    return (void *) LLVMBuildFPCast((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
}
/* Struct-value construction in SSA (no memory): start from `undef` of the
 * struct type, `insertvalue` each field at its index, pass the aggregate
 * by value (the call-site ABI classifies it from the struct type). On
 * return, `extractvalue` each field out. */
static void *kai_llvm_get_undef(void *ty) {
    return (void *) LLVMGetUndef((LLVMTypeRef) ty);
}
static void *kai_llvm_build_insertvalue(void *b, void *agg, void *elt, int64_t idx) {
    return (void *) LLVMBuildInsertValue((LLVMBuilderRef) b, (LLVMValueRef) agg,
                                         (LLVMValueRef) elt, (unsigned) idx, "");
}
static void *kai_llvm_build_extractvalue(void *b, void *agg, int64_t idx) {
    return (void *) LLVMBuildExtractValue((LLVMBuilderRef) b, (LLVMValueRef) agg,
                                          (unsigned) idx, "");
}
/* Raw `i64` arithmetic for the unboxed-Int path (KIR mode-slave to the
 * unbox pass). `op`: 0=`add`, 1=`sub`, 2=`mul` — matching the C-direct
 * oracle's `kair_a + kair_b` on a raw `int64_t`. NO `nsw`/`nuw` flags:
 * kaikai Int arithmetic WRAPS (the C emitter casts through `uint64_t`), so
 * signed overflow must be defined two's-complement wrap, not UB the
 * optimiser could exploit. The plain `LLVMBuildAdd`/`Sub`/`Mul` emit
 * wrapping ops (no-wrap flags unset). `/`→`sdiv`, `%`→`srem`, mirroring the
 * C-direct oracle: it emits a bare `a / b` and accepts the `/0` and
 * `INT_MIN/-1` UB as a separate concern, so a raw native `sdiv`/`srem` is
 * byte-for-byte parity, not a regression. Operands + result are `i64`, NOT
 * boxed Ints: no RC, so the use-after-free the boxed path hit on a multi-use
 * raw operand cannot arise. */
static void *kai_llvm_build_ibinop(void *b, int64_t op, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    switch (op) {
        case 0:  return (void *) LLVMBuildAdd(bld, la, lc, "");
        case 1:  return (void *) LLVMBuildSub(bld, la, lc, "");
        case 2:  return (void *) LLVMBuildMul(bld, la, lc, "");
        case 3:  return (void *) LLVMBuildSDiv(bld, la, lc, "");
        default: return (void *) LLVMBuildSRem(bld, la, lc, "");
    }
}
/* Raw `i64` SIGNED comparison → an `i1`. `pred`: 0=`<`, 1=`>`, 2=`<=`,
 * 3=`>=`, 4=`==`, 5=`!=` — the signed (`S*`) family, matching the C
 * `<`/`==` on a raw `int64_t`. The result is the `i1` a `condbr` consumes
 * directly, or the caller boxes via `kaix_bool` at a raw→boxed border. */
static void *kai_llvm_build_icmp(void *b, int64_t pred, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    LLVMIntPredicate p;
    switch (pred) {
        case 0:  p = LLVMIntSLT; break;
        case 1:  p = LLVMIntSGT; break;
        case 2:  p = LLVMIntSLE; break;
        case 3:  p = LLVMIntSGE; break;
        case 4:  p = LLVMIntEQ;  break;
        default: p = LLVMIntNE;  break;
    }
    return (void *) LLVMBuildICmp(bld, p, la, lc, "");
}

/* --- constants + globals --- */
static void *kai_llvm_const_i32(void *i32ty, int64_t v) {
    return (void *) LLVMConstInt((LLVMTypeRef) i32ty, (unsigned long long) v, 1);
}
/* A null `ptr` — the boxed unit/placeholder a join slot starts at before
 * a branch stores into it (it is always overwritten before a read). */
static void *kai_llvm_const_null(void *ptr_t) {
    return (void *) LLVMConstNull((LLVMTypeRef) ptr_t);
}
/* An `i8*` to a private global string constant. Two callers, two shapes:
 *  - a constructor / record-field NAME (a bare identifier, no quotes) —
 *    used as the `const char *name` a runtime ctor takes;
 *  - a `KStrV` source SPAN (quote-wrapped, with `\n`/`\t`/`\"`/`\\`/`\r`
 *    escapes) — the literal a `kaix_str(...)` boxes.
 * `kai_llvm_build_global_string` takes the bare form verbatim;
 * `kai_llvm_build_string_span` strips the outer quotes and decodes the
 * escapes (the C-direct oracle leans on the C compiler to do this for
 * `kai_str("...")`; we replicate that decode here, since the in-process
 * path has no intermediate C compiler). The builder must be positioned
 * in a block; the global is module-level + private. */
static void *kai_llvm_build_global_string(void *b, KaiValue *s) {
    /* A module-level private `[N x i8] c"..."` constant, NOT a builder
     * instruction. `LLVMBuildGlobalStringPtr` inserts a GEP into the
     * current block and, on LLVM 22 with opaque pointers + a private
     * context, produced an unserialisable value (the module crashed in
     * the printer/verifier). Mirror the mature LLVM-text emitter: add the
     * global directly to the module and hand back its pointer — the i8
     * array type comes from the builder's context so nothing is
     * cross-context. The runtime ctor reads the bytes as `const char *`. */
    LLVMBasicBlockRef bb = LLVMGetInsertBlock((LLVMBuilderRef) b);
    LLVMValueRef fn = LLVMGetBasicBlockParent(bb);
    LLVMModuleRef m = LLVMGetGlobalParent(fn);
    LLVMContextRef ctx = LLVMGetModuleContext(m);
    const char *bytes = s->as.s.bytes;
    unsigned len = (unsigned) strlen(bytes);
    LLVMValueRef init = LLVMConstStringInContext(ctx, bytes, len, 0); /* NUL-terminated */
    LLVMValueRef g = LLVMAddGlobal(m, LLVMTypeOf(init), "str");
    LLVMSetInitializer(g, init);
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    if (s) kai_decref(s);
    return (void *) g;
}
static void *kai_llvm_build_string_span(void *b, KaiValue *s) {
    const char *raw = s->as.s.bytes;
    size_t rn = strlen(raw);
    /* Strip a single pair of outer quotes if present (a KStrV span is
     * `"..."`; a triple-quoted span is `"""..."""` — handle both). */
    size_t lo = 0, hi = rn;
    if (rn >= 6 && raw[0] == '"' && raw[1] == '"' && raw[2] == '"' &&
        raw[rn-1] == '"' && raw[rn-2] == '"' && raw[rn-3] == '"') { lo = 3; hi = rn - 3; }
    else if (rn >= 2 && raw[0] == '"' && raw[rn-1] == '"') { lo = 1; hi = rn - 1; }
    /* `raw` is the verbatim source span, so an N-char escape decodes to
     * at most N bytes — `hi - lo` is a safe upper bound for the writer. */
    char *buf = (char *) malloc(hi - lo + 1);
    size_t w = 0;
    for (size_t i = lo; i < hi; i++) {
        if (raw[i] == '\\' && i + 1 < hi) {
            char c = raw[++i];
            /* Decode C99 string-literal escapes EXACTLY as the C-direct
             * oracle does: the C backend emits the span verbatim into
             * `kai_str("...")` and lets the C compiler decode it, so this
             * must match cc's interpretation byte-for-byte (simple escapes
             * + `\xH...` hex + `\ooo` octal). A divergence here is a
             * char/hex parity bug (json `\uXXXX`, regex, jwt). */
            switch (c) {
                case 'n':  buf[w++] = '\n'; break;
                case 't':  buf[w++] = '\t'; break;
                case 'r':  buf[w++] = '\r'; break;
                case 'a':  buf[w++] = '\a'; break;
                case 'b':  buf[w++] = '\b'; break;
                case 'f':  buf[w++] = '\f'; break;
                case 'v':  buf[w++] = '\v'; break;
                case '"':  buf[w++] = '"';  break;
                case '\'': buf[w++] = '\''; break;
                case '?':  buf[w++] = '?';  break;
                case '\\': buf[w++] = '\\'; break;
                case 'x': {
                    /* `\xH...`: consume every following hex digit, exactly
                     * like cc (one byte, low 8 bits of the accumulated
                     * value). A bare `\x` with no hex digit keeps `x`. */
                    int got = 0;
                    unsigned v = 0;
                    while (i + 1 < hi) {
                        char d = raw[i + 1];
                        unsigned dv;
                        if (d >= '0' && d <= '9')      dv = (unsigned)(d - '0');
                        else if (d >= 'a' && d <= 'f') dv = (unsigned)(d - 'a' + 10);
                        else if (d >= 'A' && d <= 'F') dv = (unsigned)(d - 'A' + 10);
                        else break;
                        v = (v << 4) | dv;
                        i++;
                        got = 1;
                    }
                    buf[w++] = got ? (char)(v & 0xFF) : 'x';
                    break;
                }
                case '0': case '1': case '2': case '3':
                case '4': case '5': case '6': case '7': {
                    /* `\ooo`: 1–3 octal digits, low 8 bits — cc semantics.
                     * `\0` alone (no further octal digit) is the NUL byte. */
                    unsigned v = (unsigned)(c - '0');
                    int n = 1;
                    while (n < 3 && i + 1 < hi && raw[i + 1] >= '0' && raw[i + 1] <= '7') {
                        v = (v << 3) | (unsigned)(raw[i + 1] - '0');
                        i++;
                        n++;
                    }
                    buf[w++] = (char)(v & 0xFF);
                    break;
                }
                default: buf[w++] = c; break;   /* unknown escape: keep the char */
            }
        } else {
            buf[w++] = raw[i];
        }
    }
    buf[w] = '\0';
    /* Module-level private constant (NOT a builder instruction) — same
     * fix as `kai_llvm_build_global_string`: `LLVMBuildGlobalStringPtr`
     * yields an unserialisable value on LLVM 22 opaque-ptr + private
     * context. `w` is the de-escaped length (an embedded `\0` is kept). */
    LLVMBasicBlockRef bb = LLVMGetInsertBlock((LLVMBuilderRef) b);
    LLVMValueRef fn = LLVMGetBasicBlockParent(bb);
    LLVMModuleRef m = LLVMGetGlobalParent(fn);
    LLVMContextRef ctx = LLVMGetModuleContext(m);
    LLVMValueRef init = LLVMConstStringInContext(ctx, buf, (unsigned) w, 0);
    LLVMValueRef g = LLVMAddGlobal(m, LLVMTypeOf(init), "str");
    LLVMSetInitializer(g, init);
    LLVMSetGlobalConstant(g, 1);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, 1);
    free(buf);
    if (s) kai_decref(s);
    return (void *) g;
}

/* A module-level zero-initialised internal global of type `ty`, returning
 * its address. The native default-handler install needs the `EvX` blob and
 * the `KaiEvidence` node to OUTLIVE `kai_main_install_defaults`'s frame (the
 * evidence stays pushed for the whole program), so they are module globals,
 * not entry-block allocas — the C-direct oracle emits `static EvStdout
 * _kai_default_ev_stdout;` for exactly this reason. Internal linkage + a
 * zero initialiser match the C `static` (file-scope, zero-init). The type
 * comes from the module's context so nothing is cross-context. */
static void *kai_llvm_add_global_zeroed(void *m, void *ty, KaiValue *name) {
    LLVMValueRef g = LLVMAddGlobal((LLVMModuleRef) m, (LLVMTypeRef) ty, name->as.s.bytes);
    LLVMSetInitializer(g, LLVMConstNull((LLVMTypeRef) ty));
    LLVMSetLinkage(g, LLVMInternalLinkage);
    if (name) kai_decref(name);
    return (void *) g;
}

/* (The `i32 variant_tag` reader the emitted object calls for a `KTagOf`
 * — `kaix_variant_tag_of` — is a RUNTIME symbol in stage0/runtime_llvm.c
 * next to `kaix_is_variant_tag`, not a C-API builder prim: the native
 * object links it, the compiler does not call it.) */

/* --- DWARF debug info (#500) ---------------------------------------------
 * The native backend emits DWARF line tables in --debug so `lldb`/`gdb`
 * break on kaikai source lines and a panic resolves to `<file>.kai:<line>`
 * via `atos`/`addr2line`. The metadata lives in the off-RC `KaiNativeCtx`
 * (same vehicle as every other LLVM handle — a `DIBuilderRef` must never
 * ride a kaikai record/list). The kaikai walk calls six high-level prims;
 * the per-DI-node LLVM sequence stays in C so the emit_native code reads as
 * "enable / open subprogram / set line / finalize", not raw DIBuilder.
 *
 * Scope: one DIFile + DICompileUnit per module, one DISubprogram per fn (a
 * void() subroutine type — enough for line tables; we do not describe
 * parameter/local types, which is out of scope per #500). A `set_loc`
 * attaches the current subprogram as the location scope. Every prim is a
 * NO-OP when DI was never enabled (`c->dib == NULL`), so a release/default
 * build that never calls `native_di_enable` is byte-identical. */

/* Enable DWARF for module `m`'s ctx: build the DIBuilder, the DIFile from
 * (filename, directory), and the compile unit, and set the module's DWARF
 * version + debug-info-version flags (without them the backend drops the
 * metadata silently). Idempotent — a second call is a no-op. */
static KaiValue *kai_native_di_enable(void *cv, KaiValue *fnamev, KaiValue *dirv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (c->dib) { kai_decref(fnamev); kai_decref(dirv); return kai_unit(); }
    LLVMModuleRef m = (LLVMModuleRef) c->m;
    LLVMContextRef ctx = LLVMGetModuleContext(m);
    /* DWARF needs these module flags; emit them once. Values match what
     * clang sets for `-g` on the LLVM 18 line. */
    LLVMMetadataRef dwarf_ver = LLVMValueAsMetadata(
        LLVMConstInt(LLVMInt32TypeInContext(ctx), 4, 0));
    LLVMAddModuleFlag(m, LLVMModuleFlagBehaviorWarning, "Dwarf Version", 13, dwarf_ver);
    LLVMMetadataRef di_ver = LLVMValueAsMetadata(
        LLVMConstInt(LLVMInt32TypeInContext(ctx), (unsigned) LLVMDebugMetadataVersion(), 0));
    LLVMAddModuleFlag(m, LLVMModuleFlagBehaviorWarning, "Debug Info Version", 18, di_ver);

    c->dib = LLVMCreateDIBuilder(m);
    const char *fname = fnamev->as.s.bytes;
    const char *dir = dirv->as.s.bytes;
    c->difile = LLVMDIBuilderCreateFile((LLVMDIBuilderRef) c->dib,
        fname, strlen(fname), dir, strlen(dir));
    c->dicu = LLVMDIBuilderCreateCompileUnit((LLVMDIBuilderRef) c->dib,
        LLVMDWARFSourceLanguageC, (LLVMMetadataRef) c->difile,
        "kaikai", 6, /*isOptimized=*/0, "", 0, /*RuntimeVer=*/0,
        "", 0, LLVMDWARFEmissionFull, /*DWOId=*/0,
        /*SplitDebugInlining=*/0, /*DebugInfoForProfiling=*/0, "", 0, "", 0);
    kai_decref(fnamev); kai_decref(dirv);
    return kai_unit();
}

/* 1 when DWARF is enabled on this ctx (the --debug walk gates every DI
 * call on it), 0 otherwise — so the kaikai walk reads `if di_enabled`. */
static int64_t kai_native_di_enabled(void *cv) {
    return ((KaiNativeCtx *) cv)->dib ? 1 : 0;
}

/* Open a DISubprogram for the current fn `fnval` named `name` at source
 * `line`, attach it (LLVMSetSubprogram), and record it as the current
 * location scope. No-op when DI is off. */
static KaiValue *kai_native_di_subprogram(void *cv, void *fnval, KaiValue *namev, int64_t line) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (!c->dib) { kai_decref(namev); return kai_unit(); }
    /* A `void ()` subroutine type — line tables need a type but not the
     * parameter shapes (out of scope per #500). */
    LLVMMetadataRef subty = LLVMDIBuilderCreateSubroutineType(
        (LLVMDIBuilderRef) c->dib, (LLVMMetadataRef) c->difile, NULL, 0, LLVMDIFlagZero);
    const char *name = namev->as.s.bytes;
    unsigned ln = (line > 0) ? (unsigned) line : 1u;
    LLVMMetadataRef sp = LLVMDIBuilderCreateFunction(
        (LLVMDIBuilderRef) c->dib, (LLVMMetadataRef) c->dicu,
        name, strlen(name), name, strlen(name),
        (LLVMMetadataRef) c->difile, ln, subty,
        /*IsLocalToUnit=*/0, /*IsDefinition=*/1, /*ScopeLine=*/ln,
        LLVMDIFlagZero, /*IsOptimized=*/0);
    LLVMSetSubprogram((LLVMValueRef) fnval, sp);
    c->disub = sp;
    kai_decref(namev);
    return kai_unit();
}

/* Set the builder's current debug location to (line, col) under the
 * current subprogram scope. No-op when DI is off or no subprogram is open
 * (a synthetic fn with no source). The walk calls this before each
 * source-bearing instruction; `set_loc(0,0)` style positions never reach
 * here (the walk only calls on a real KAt). */
static KaiValue *kai_native_di_set_loc(void *cv, int64_t line, int64_t col) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (!c->dib || !c->disub) return kai_unit();
    LLVMContextRef ctx = LLVMGetModuleContext((LLVMModuleRef) c->m);
    LLVMMetadataRef loc = LLVMDIBuilderCreateDebugLocation(
        ctx, (unsigned) line, (unsigned) col, (LLVMMetadataRef) c->disub, NULL);
    LLVMSetCurrentDebugLocation2((LLVMBuilderRef) c->b, loc);
    return kai_unit();
}

/* Clear the builder's current debug location (the prologue / synthetic
 * instructions carry none). No-op when DI is off. */
static KaiValue *kai_native_di_clear_loc(void *cv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (!c->dib) return kai_unit();
    LLVMSetCurrentDebugLocation2((LLVMBuilderRef) c->b, NULL);
    return kai_unit();
}

/* Resolve every temporary DI node — MUST run after the whole module is
 * built and BEFORE verify (an unfinalized DIBuilder leaves forward-ref
 * placeholders the verifier rejects). No-op when DI is off. */
static KaiValue *kai_native_di_finalize(void *cv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (c->dib) LLVMDIBuilderFinalize((LLVMDIBuilderRef) c->dib);
    return kai_unit();
}

/* --- optimisation pass pipeline (L4, issue #498) ---
 * Run the LLVM New-PM pipeline in-process on the module before codegen,
 * matching the `-O2` the out-of-process C/LLVM-text paths get from
 * `clang -O2`. Two profiles selected by `KAI_NATIVE_OPT`:
 *   release (default / "2") — `default<O2>`: the per-module pipeline
 *     `clang -O2` builds (inlining, vectorisation, unrolling); same
 *     pass set, so native matches the C path's runtime perf.
 *   debug ("0") — `default<O0>`: minimum legalisation, no inlining,
 *     fast compile, symbols kept. Selected by `bin/kai --debug`.
 * Levels "1"/"3"/"s"/"z" map straight to `default<O1|O3|Os|Oz>`; any
 * other value falls back to O2. The pipeline string format is the same
 * as `opt -passes=` and stable across the LLVM 18.x series.
 *
 * Runs AFTER verify (a pass set assumes verified IR; verify catches a
 * codegen bug with a clear message before a pass turns it into an opaque
 * LLVM crash) and reuses the SAME TargetMachine the emit step builds, so
 * the pipeline's TargetTransformInfo cost model matches the emission
 * target exactly (asu review). Returns 0 on success, non-zero on a pass
 * error (surfaced like a verify failure). */
static const char *kai_llvm_pass_pipeline(void) {
    const char *lvl = getenv("KAI_NATIVE_OPT");
    if (!lvl || !lvl[0]) return "default<O2>";   /* default: release */
    if (strcmp(lvl, "0") == 0) return "default<O0>";
    if (strcmp(lvl, "1") == 0) return "default<O1>";
    if (strcmp(lvl, "3") == 0) return "default<O3>";
    if (strcmp(lvl, "s") == 0) return "default<Os>";
    if (strcmp(lvl, "z") == 0) return "default<Oz>";
    return "default<O2>";                         /* "2" and unknowns */
}

static int64_t kai_llvm_run_passes(LLVMModuleRef m, LLVMTargetMachineRef tm) {
    const char *pipeline = kai_llvm_pass_pipeline();
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef e = LLVMRunPasses(m, pipeline, tm, opts);
    LLVMDisposePassBuilderOptions(opts);
    if (e) {
        char *msg = LLVMGetErrorMessage(e);   /* consumes e + allocates msg */
        fprintf(stderr, "kai: native opt pass pipeline (%s) failed: %s\n",
                pipeline, msg ? msg : "?");
        LLVMDisposeErrorMessage(msg);         /* frees msg; do NOT ConsumeError(e) */
        return 1;
    }
    return 0;
}

/* --- runtime bitcode link (P2, docs/native-codegen-perf-plan.md §P2) ---
 *
 * The native walk emits the runtime ops (`kaix_cons`, `kaix_variant_arg`,
 * the list spine, the arithmetic helpers) as external `declare`s; their
 * bodies live in `runtime_llvm.c`, compiled and linked by `cc` AFTER the
 * in-process `default<O2>` pass. With no LTO, O2 sees opaque call barriers
 * it cannot inline through — so a heap-bound loop (`list_fold`, the
 * rb-tree) pays a real call per `kaix_cons` even at O2.
 *
 * This step closes that gap WITHOUT an LTO toolchain dependency or a second
 * source of the runtime: `runtime_llvm.c` is compiled to LLVM bitcode at
 * build time (`tools/gen-runtime-bc.sh`, clang 18, the same `-I stage2 -I
 * stage0` resolution the cc link uses, so its `<runtime.h>` binds to the
 * Koka runtime exactly as the C path does), and that bitcode is
 * `LLVMLinkModules2`'d into the in-process module BEFORE the opt pipeline
 * runs. O2 then sees the runtime BODIES and inlines/specialises them into
 * the hot loop.
 *
 * Symbol model (asu-reviewed): a full link (Model X), NOT an
 * `available_externally` graft (Model Y, the two-convergent-runtimes
 * anti-pattern that killed the text-LLVM backend). After the link the
 * runtime defs are merged physically into this module; we `internalize`
 * every definition except `main` (the OS entry point the linker resolves)
 * so the inliner can DCE the merged bodies it folds away. Because the
 * bitcode supplies `main` + all `kaix_*` + the kaikai entry hooks, the
 * resulting object is SELF-CONTAINED: the driver drops `runtime_llvm.c`
 * from the final `cc` link (re-linking it would be a duplicate symbol).
 *
 * The bitcode path comes from `KAI_NATIVE_RUNTIME_BC` (the `bin/kai`
 * wrapper resolves it next to `runtime_llvm.c`, mirroring how it resolves
 * `RUNTIME_LLVM_C`; the stage2 Makefile builds it). When the env var is
 * unset or the file is absent (a build without clang 18, so no bitcode was
 * produced), this is a NO-OP and the driver falls back to the legacy
 * `cc`-links-runtime_llvm.c path — correct, just unoptimised. The opt level
 * is unchanged.
 *
 * MUST run after the module's target triple + data layout are set (this
 * function copies them onto the bitcode source before linking, so the merge
 * is clean and layout-correct) and BEFORE the opt pipeline. Returns 0 on
 * success OR no-op; non-zero only on a real parse/link failure (a corrupt or
 * incompatible bitcode), which the caller surfaces like a verify failure
 * rather than emitting a half-linked module. */
static void kai_llvm_internalize_except_main(LLVMModuleRef m) {
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;            /* nothing to internalise */
        const char *nm = LLVMGetValueName(f);
        if (nm && strcmp(nm, "main") == 0) continue;   /* OS entry point — keep external */
        LLVMSetLinkage(f, LLVMInternalLinkage);
    }
    /* Globals defined by the runtime (string/variant-head tables, the
     * default-evidence blobs) are referenced only from inside this now-merged
     * module; internalise them too so globalDCE under O2 can drop the unused
     * ones. A global the emitted code still references survives (it has a
     * user); one the inliner folded away does not. */
    for (LLVMValueRef g = LLVMGetFirstGlobal(m); g; g = LLVMGetNextGlobal(g)) {
        if (LLVMIsDeclaration(g)) continue;
        LLVMSetLinkage(g, LLVMInternalLinkage);
    }
}

static int64_t kai_llvm_link_runtime_bc(void *m) {
    const char *bc_path = getenv("KAI_NATIVE_RUNTIME_BC");
    if (!bc_path || !bc_path[0]) return 0;             /* opt-out: legacy cc-links path */

    LLVMMemoryBufferRef buf = NULL;
    char *err = NULL;
    if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &err)) {
        /* File named but unreadable — not a no-op situation (the driver set
         * the var, so it expected a bitcode). Fail loudly. */
        fprintf(stderr, "kai: native runtime bitcode unreadable (%s): %s\n",
                bc_path, err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        return 1;
    }

    /* Parse INTO the destination module's context — LLVMLinkModules2 rejects
     * a cross-context source. LLVMParseIRInContext takes ownership of `buf`. */
    LLVMContextRef ctx = LLVMGetModuleContext((LLVMModuleRef) m);
    LLVMModuleRef src = NULL;
    err = NULL;
    if (LLVMParseIRInContext(ctx, buf, &src, &err)) {
        fprintf(stderr, "kai: native runtime bitcode parse failed (%s): %s\n",
                bc_path, err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        return 1;
    }

    /* Reconcile the source's triple + data layout to the destination's BEFORE
     * the link. The bitcode was produced by `clang -O2` with its own SDK
     * triple (e.g. `arm64-apple-macosx16.0.0`), while the in-process module
     * carries `LLVMGetDefaultTargetTriple()` (e.g. `...-darwin25.5.0`). The
     * two are ABI-identical (same arch, same host LLVM), but a literal string
     * mismatch makes LLVMLinkModules2 emit a noisy "different target triples"
     * warning and would, on a real cross-target difference, keep the
     * destination's silently. We set them equal so the link is clean and the
     * match is guaranteed on every platform regardless of how the bitcode was
     * generated. The destination's triple/layout were set by the caller from
     * the emit TargetMachine just above this call. */
    {
        const char *dst_triple = LLVMGetTarget((LLVMModuleRef) m);
        if (dst_triple) LLVMSetTarget(src, dst_triple);
        LLVMSetModuleDataLayout(src, LLVMGetModuleDataLayout((LLVMModuleRef) m));
    }

    /* LLVMLinkModules2 consumes (and disposes) `src`. Returns 1 on error. */
    if (LLVMLinkModules2((LLVMModuleRef) m, src)) {
        fprintf(stderr, "kai: native runtime bitcode link failed (%s)\n", bc_path);
        return 1;
    }

    kai_llvm_internalize_except_main((LLVMModuleRef) m);
    return 0;
}

/* --- object emission --- */
/* Verify the module, then emit it as a native object file at `path`
 * using the host target machine. Returns 0 on success, non-zero on a
 * verify or codegen failure (the driver surfaces the error and aborts).
 * One process: no `.ll` text, no `clang` subprocess. */
static int64_t kai_llvm_emit_object(void *m, KaiValue *path) {
    const char *out = path->as.s.bytes;
    int64_t rc = 0;
    char *err = NULL;

    /* Debug hook (KIR native walk): dump the in-memory module IR to the
     * path in KAI_NATIVE_DUMP_IR before verify, so a malformed module can
     * be inspected. Off by default; never affects the emitted object. */
    {
        const char *ir_path = getenv("KAI_NATIVE_DUMP_IR");
        if (ir_path && ir_path[0]) {
            char *ir_err = NULL;
            LLVMPrintModuleToFile((LLVMModuleRef) m, ir_path, &ir_err);
            if (ir_err) LLVMDisposeMessage(ir_err);
        }
    }

    if (LLVMVerifyModule((LLVMModuleRef) m, LLVMReturnStatusAction, &err)) {
        fprintf(stderr, "kai: native module verify failed: %s\n", err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        if (path) kai_decref(path);
        return 1;
    }
    if (err) { LLVMDisposeMessage(err); err = NULL; }

    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMTargetRef target = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &err)) {
        fprintf(stderr, "kai: native target lookup failed: %s\n", err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        if (path) kai_decref(path);
        return 1;
    }
    char *cpu = LLVMGetHostCPUName();
    char *features = LLVMGetHostCPUFeatures();
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, cpu ? cpu : "", features ? features : "",
        LLVMCodeGenLevelDefault, LLVMRelocPIC, LLVMCodeModelDefault);
    if (cpu) LLVMDisposeMessage(cpu);
    if (features) LLVMDisposeMessage(features);
    LLVMSetTarget((LLVMModuleRef) m, triple);
    LLVMSetModuleDataLayout((LLVMModuleRef) m, LLVMCreateTargetDataLayout(tm));

    /* P2: link the runtime as bitcode BEFORE opt, so default<O2> sees the
     * `kaix_*` bodies and inlines them (no-op when KAI_NATIVE_RUNTIME_BC is
     * unset — the legacy cc-links-runtime_llvm.c path). Runs after the
     * triple/datalayout are set (they must match the bitcode's) and before
     * the pipeline. A real link failure aborts before EmitToFile. */
    if (kai_llvm_link_runtime_bc(m)) {
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeMessage(triple);
        if (path) kai_decref(path);
        return 1;
    }

    /* L4 (issue #498): optimise the module in-process before codegen.
     * Default `default<O2>` for parity with the clang `-O2` the C/LLVM-
     * text paths get; `KAI_NATIVE_OPT=0` (bin/kai --debug) drops to O0.
     * A pass error aborts before EmitToFile (no half-optimised object). */
    if (kai_llvm_run_passes((LLVMModuleRef) m, tm)) {
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeMessage(triple);
        if (path) kai_decref(path);
        return 1;
    }

    if (LLVMTargetMachineEmitToFile(tm, (LLVMModuleRef) m, out, LLVMObjectFile, &err)) {
        fprintf(stderr, "kai: native object emit failed: %s\n", err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        rc = 1;
    }

    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);
    if (path) kai_decref(path);
    return rc;
}
#else /* !KAI_LLVM */
/* Default / bootstrap build: libLLVM is not linked, so the C-API
 * forwarders are stubs. They exist only so the emitted compiler (which
 * always contains the `emit_native` code path) LINKS; calling
 * `--emit=native` on a kaic2 built without `KAI_LLVM=1` aborts here with
 * a clear message instead of a link error. The default backend never
 * reaches these (it dispatches to the C-text emitter). */
static void *kai_llvm_native_unavailable(void) {
    fprintf(stderr,
        "kai: the native (in-process libLLVM) backend is not built into this "
        "compiler.\n     Rebuild stage2 with `make KAI_LLVM=1` to enable "
        "`--emit=native`.\n");
    exit(1);
    return NULL;
}
static void *kai_llvm_module_new(KaiValue *name) { (void) name; return kai_llvm_native_unavailable(); }
/* Parte B native-context stubs. */
static void *kai_native_ctx_new(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_b(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_m(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_ptrt(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_i64t(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_i32t(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_voidt(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_f64t(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_const_real(void *t, double d) { (void) t; (void) d; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_fbinop(void *b, int64_t op, void *a, void *c) { (void) b; (void) op; (void) a; (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_fcmp(void *b, int64_t p, void *a, void *c) { (void) b; (void) p; (void) a; (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_fneg(void *b, void *a) { (void) b; (void) a; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_logical(void *b, int64_t op, void *a, void *c) { (void) b; (void) op; (void) a; (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_lnot(void *b, void *a) { (void) b; (void) a; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_zext_i1_i32(void *b, void *v, void *t) { (void) b; (void) v; (void) t; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_ibinop(void *b, int64_t op, void *a, void *c) { (void) b; (void) op; (void) a; (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_icmp(void *b, int64_t p, void *a, void *c) { (void) b; (void) p; (void) a; (void) c; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_fnval(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static KaiValue *kai_native_ctx_set_fnval(void *c, void *fn) { (void) c; (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_ctx_ok(void *c) { (void) c; kai_llvm_native_unavailable(); return 0; }
static KaiValue *kai_native_ctx_fail(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_ctx_begin_fn(void *c, void *fn) { (void) c; (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_ctx_end_fn(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_ctx_add_frame_slot(void *c, KaiValue *s, KaiValue *e) { (void) c; (void) s; (void) e; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_ctx_frame_slot_count(void *c, KaiValue *s) { (void) c; (void) s; kai_llvm_native_unavailable(); return 0; }
static KaiValue *kai_native_ctx_frame_slot_eff(void *c, KaiValue *s, int64_t j) { (void) c; (void) s; (void) j; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_ctx_frame_slot_index(void *c, KaiValue *s, KaiValue *e) { (void) c; (void) s; (void) e; kai_llvm_native_unavailable(); return -1; }
static KaiValue *kai_native_ctx_add_reg(void *c, KaiValue *n, void *a, int64_t s) { (void) c; (void) n; (void) a; (void) s; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_native_ctx_find_reg(void *c, KaiValue *n) { (void) c; (void) n; return kai_llvm_native_unavailable(); }
static int64_t kai_native_ctx_reg_slot(void *c, KaiValue *n) { (void) c; (void) n; kai_llvm_native_unavailable(); return -1; }
static void *kai_native_ctx_reg_at(void *c, int64_t i) { (void) c; (void) i; return kai_llvm_native_unavailable(); }
static int64_t kai_native_ctx_reg_slot_at(void *c, int64_t i) { (void) c; (void) i; kai_llvm_native_unavailable(); return -1; }
static KaiValue *kai_native_ctx_add_block(void *c, KaiValue *l, void *bb) { (void) c; (void) l; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_native_ctx_find_block(void *c, KaiValue *l) { (void) c; (void) l; return kai_llvm_native_unavailable(); }
static void *kai_native_ctx_first_block(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_int64_type(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_llvm_int32_type(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_llvm_ptr_type(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_llvm_void_type(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_llvm_float_type(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static void *kai_llvm_int_type(void *m, int64_t bits) { (void) m; (void) bits; return kai_llvm_native_unavailable(); }
static void *kai_llvm_struct_type(void *m, void *buf) { (void) m; (void) buf; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_struct_gep(void *b, void *s, void *p, int64_t i) { (void) b; (void) s; (void) p; (void) i; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_add_byval_decl(void *m, void *fn, int64_t ix, void *s) { (void) m; (void) fn; (void) ix; (void) s; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_add_byval_call(void *m, void *c, int64_t ix, void *s) { (void) m; (void) c; (void) ix; (void) s; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_build_trunc(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_sext(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_zext(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_fpcast(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_get_undef(void *ty) { (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_insertvalue(void *b, void *agg, void *elt, int64_t idx) { (void) b; (void) agg; (void) elt; (void) idx; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_extractvalue(void *b, void *agg, int64_t idx) { (void) b; (void) agg; (void) idx; return kai_llvm_native_unavailable(); }
static void *kai_llvm_fn_type_0(void *ret) { (void) ret; return kai_llvm_native_unavailable(); }
static void *kai_llvm_fn_type_1(void *ret, void *p0) { (void) ret; (void) p0; return kai_llvm_native_unavailable(); }
static void *kai_llvm_add_function(void *m, KaiValue *name, void *fnty) { (void) m; (void) name; (void) fnty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_append_block(void *m, void *fn, KaiValue *name) { (void) m; (void) fn; (void) name; return kai_llvm_native_unavailable(); }
static void *kai_llvm_builder_new(void *m) { (void) m; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_position_at_end(void *b, void *bb) { (void) b; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_const_int(void *i64ty, int64_t v) { (void) i64ty; (void) v; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_0(void *b, void *fn, void *fnty) { (void) b; (void) fn; (void) fnty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_1(void *b, void *fn, void *fnty, void *a0) { (void) b; (void) fn; (void) fnty; (void) a0; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_build_ret(void *b, void *v) { (void) b; (void) v; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_build_ret_void(void *b) { (void) b; kai_llvm_native_unavailable(); return kai_unit(); }
/* Parte B generic-walk stubs (same contract: link, then abort on use). */
static void *kai_llvm_fn_type_boxed(void *p, int64_t n) { (void) p; (void) n; return kai_llvm_native_unavailable(); }
static void *kai_llvm_fn_type_n(void *r, void *buf) { (void) r; (void) buf; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_n(void *b, void *fn, void *t, void *buf) { (void) b; (void) fn; (void) t; (void) buf; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_position_at_start(void *b, void *bb) { (void) b; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_get_or_declare_fn(void *m, KaiValue *nm, void *t) { (void) m; (void) nm; (void) t; return kai_llvm_native_unavailable(); }
static void *kai_llvm_get_param(void *fn, int64_t i) { (void) fn; (void) i; return kai_llvm_native_unavailable(); }
static void *kai_llvm_array_type(void *el, int64_t n) { (void) el; (void) n; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_array_gep(void *b, void *t, void *a, void *i) { (void) b; (void) t; (void) a; (void) i; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_load_arg(void *b, void *a, void *t, int64_t i) { (void) b; (void) a; (void) t; (void) i; return kai_llvm_native_unavailable(); }
static void *kai_llvm_buf_new(void) { return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_buf_push(void *buf, void *h) { (void) buf; (void) h; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_buf_free(void *buf) { (void) buf; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_llvm_buf_len(void *buf) { (void) buf; kai_llvm_native_unavailable(); return 0; }
static void *kai_llvm_buf_get(void *buf, int64_t i) { (void) buf; (void) i; return kai_llvm_native_unavailable(); }
static void *kai_llvm_const_i32(void *t, int64_t v) { (void) t; (void) v; return kai_llvm_native_unavailable(); }
static void *kai_llvm_const_null(void *t) { (void) t; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_global_string(void *b, KaiValue *s) { (void) b; (void) s; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_string_span(void *b, KaiValue *s) { (void) b; (void) s; return kai_llvm_native_unavailable(); }
static void *kai_llvm_add_global_zeroed(void *m, void *t, KaiValue *nm) { (void) m; (void) t; (void) nm; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_alloca(void *b, void *t, KaiValue *nm) { (void) b; (void) t; (void) nm; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_alloca_entry(void *c, void *t, KaiValue *nm) { (void) c; (void) t; (void) nm; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_array_alloca(void *b, void *et, void *c, KaiValue *nm) { (void) b; (void) et; (void) c; (void) nm; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_build_store(void *b, void *v, void *p) { (void) b; (void) v; (void) p; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_build_load(void *b, void *t, void *p) { (void) b; (void) t; (void) p; return kai_llvm_native_unavailable(); }
static void *kai_llvm_get_named_global(void *m, KaiValue *nm) { (void) m; (void) nm; return kai_llvm_native_unavailable(); }
static int32_t kai_llvm_handle_is_null(void *h) { (void) h; return 1; }
static KaiValue *kai_llvm_build_br(void *b, void *bb) { (void) b; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_build_cond_br(void *b, void *c, void *t, void *e) { (void) b; (void) c; (void) t; (void) e; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_build_switch(void *b, void *v, void *d, int64_t n) { (void) b; (void) v; (void) d; (void) n; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_add_case(void *sw, void *on, void *bb) { (void) sw; (void) on; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_build_unreachable(void *b) { (void) b; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_llvm_build_icmp_ne_zero(void *b, void *v, void *t) { (void) b; (void) v; (void) t; return kai_llvm_native_unavailable(); }
static int64_t kai_llvm_emit_object(void *m, KaiValue *path) { (void) m; (void) path; kai_llvm_native_unavailable(); return 1; }
/* DWARF DI stubs (#500): unreachable on the C-only path (the native walk
 * that calls them never runs), so they just satisfy the link. */
static KaiValue *kai_native_di_enable(void *c, KaiValue *f, KaiValue *d) { (void) c; (void) f; (void) d; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_di_enabled(void *c) { (void) c; kai_llvm_native_unavailable(); return 0; }
static KaiValue *kai_native_di_subprogram(void *c, void *fn, KaiValue *n, int64_t l) { (void) c; (void) fn; (void) n; (void) l; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_set_loc(void *c, int64_t l, int64_t col) { (void) c; (void) l; (void) col; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_clear_loc(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_finalize(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
#endif /* KAI_LLVM */

#endif /* KAI_RUNTIME_H */
