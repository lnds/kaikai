/* Fragment of runtime.h; include runtime.h, never this file. */
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
    /* Cross-thread-handle rule: KAI_FIBER and KAI_PID are runtime-owned
     * handles to a unit of execution or a channel reachable from more than
     * one scheduler thread, so their rc is atomic by construction.
     * Everything the user's program builds as data stays non-atomic and
     * crosses threads only by copy-on-send. */
    KAI_FIBER,      /* m8 #3: Spawn / Fiber[T] handle (opaque) */
    KAI_PID,        /* m8 #7: Actor[Msg] / Pid[Msg] handle (opaque) */
    KAI_BYTE,         /* Lane 4 (#473): unsigned 8-bit integer, nominal */
    KAI_INT32,        /* numeric lane A: signed 32-bit, nominal */
    KAI_UINT32,       /* numeric lane A: unsigned 32-bit, nominal */
    KAI_UINT64,       /* numeric lane A: unsigned 64-bit, nominal */
    KAI_INT128,       /* numeric lane A: signed 128-bit, nominal */
    KAI_FOREIGN,      /* FFI v2 (#417): opaque C handle (extern "C" opaque T).
                       * Parks a raw `void *` the kaikai side threads through
                       * but never inspects. RC manages the box; the parked
                       * pointer is NEVER freed (no Drop integration — the
                       * driver owns that lifetime). Identity-compared. */
    KAI_VEC,          /* Vec[T]: pure value vector over one flat growable
                       * buffer. One RC header for the whole vector; elements
                       * stored unboxed (raw scalars / inline records) when
                       * the element shape allows, boxed otherwise. Writes
                       * mutate in place when the buffer is uniquely owned
                       * (rc == 1) and copy-on-write when shared. */
    KAI_RANGE,        /* [a..b] / [a..b..s]: an Int cons-list stored as its
                       * generator {from, to, step} — a REPRESENTATION of
                       * `[Int]`, not a distinct type (same trick as KAI_VEC
                       * under the sequence type). Consumers either iterate
                       * the generator directly or normalise one cell at a
                       * time via kai_seq_norm; a KAI_RANGE must never reach
                       * an `as.cons` access un-normalised. No interior
                       * pointers: dup/drop are plain. */
    KAI_CONT,         /* A one-shot continuation: names a suspended stack
                       * segment until a resume or discontinue empties it.
                       * Dropping a full box discontinues the segment. */
    KAI_SEQ           /* Seq[t]: a lazy cell. One slot holding the thunk that
                       * computes its step, then the step; `variant_tag`
                       * says which. Never leaves the fiber that made it. */
} KaiTag;

/* The state of a KAI_SEQ, kept in `variant_tag`: its slot is the thunk
 * that computes the step, that thunk while it runs, or the step. */
enum { KAI_SEQ_THUNK = 0, KAI_SEQ_RUNNING = 1, KAI_SEQ_DONE = 2,
       KAI_SEQ_FILTER = 3, KAI_SEQ_FILTERING = 4 };

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
#define KAI_HEAD_VEC         14
#define KAI_HEAD_OPTION      16
#define KAI_HEAD_RESULT      17
#define KAI_HEAD_SIGNAL      18
#define KAI_HEAD_PROCESS_EXIT 19
#define KAI_HEAD_PAIR        20
#define KAI_HEAD_TRIPLE      21
#define KAI_HEAD_QUAD        22
#define KAI_USER_HEAD_TAG_BASE 23

/* Protocol IDs — the stdlib protocols; user protocols start at
 * KAI_USER_PROTO_ID_BASE.
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
#define KAI_PROTO_LAYOUT       13
#define KAI_PROTO_JSON         14
#define KAI_PROTO_NEG          15
#define KAI_USER_PROTO_ID_BASE 16

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
 * the bootstrap table covers the 12 reserved builtin variants
 * (Some/None -> Option, Ok/Err -> Result, Sig* -> Signal,
 * Exited/Signaled -> ProcessExit). */
static const int32_t kai_variant_to_head_bootstrap[12] = {
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
    /* 11 */ KAI_HEAD_SIGNAL,        /* SigWinch */
};

/* Tag-keyed runtime metadata (variant->head map, impl table, name/mask
 * tables, caches below) is process-global by design: a variant built in
 * one translation unit is walked, freed, and dispatched in another.
 * Under KAI_SEPARATE_COMPILATION each table therefore gets one shared
 * copy (extern everywhere, defined by the KAI_RUNTIME_OWNER TU), like
 * the RC free-list pools. A per-TU copy is not just waste: the reader
 * TU would see an empty table and free through the wrong slot mask. */
#if defined(KAI_SEPARATE_COMPILATION)
extern const int32_t *kai_variant_to_head;
extern int32_t        kai_variant_to_head_len;
#  if defined(KAI_RUNTIME_OWNER)
const int32_t *kai_variant_to_head     = kai_variant_to_head_bootstrap;
int32_t        kai_variant_to_head_len = 12;
#  endif
#else
static const int32_t *kai_variant_to_head     = kai_variant_to_head_bootstrap;
static int32_t        kai_variant_to_head_len = 12;
#endif

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

#if defined(KAI_SEPARATE_COMPILATION)
extern KaiImplSlot *kai_impl_table;
extern int32_t      kai_impl_cap;
extern int32_t      kai_impl_count;
#  if defined(KAI_RUNTIME_OWNER)
KaiImplSlot *kai_impl_table  = NULL;
int32_t      kai_impl_cap    = 0;
int32_t      kai_impl_count  = 0;
#  endif
#else
static KaiImplSlot *kai_impl_table  = NULL;
static int32_t      kai_impl_cap    = 0;
static int32_t      kai_impl_count  = 0;
#endif

/* One byte per head tag: set when the head has an `Eq.eq` row. The
 * structural compare asks the table only for those heads, so a value
 * whose type has no `impl Eq` costs it no probe. */
#if defined(KAI_SEPARATE_COMPILATION)
extern uint8_t *kai_eq_heads;
extern int32_t  kai_eq_heads_len;
#  if defined(KAI_RUNTIME_OWNER)
uint8_t *kai_eq_heads     = NULL;
int32_t  kai_eq_heads_len = 0;
#  endif
#else
static uint8_t *kai_eq_heads     = NULL;
static int32_t  kai_eq_heads_len = 0;
#endif

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
KAI_RT_SHARED_INLINE void *kai_lookup_impl(int32_t proto_id, int32_t op_id, int32_t head_tag) {
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
    int32_t eq_len = 0;
    for (int32_t k = 0; k < n; ++k) {
        kai_impl_insert(entries[k].proto_id, entries[k].op_id, entries[k].head_tag, entries[k].fn);
        if (entries[k].proto_id == KAI_PROTO_EQ && entries[k].head_tag >= eq_len) eq_len = entries[k].head_tag + 1;
    }
    if (kai_eq_heads != NULL) free(kai_eq_heads);
    kai_eq_heads     = (uint8_t *) calloc((size_t) (eq_len > 0 ? eq_len : 1), 1);
    kai_eq_heads_len = eq_len;
    for (int32_t k = 0; k < n; ++k) {
        if (entries[k].proto_id == KAI_PROTO_EQ && entries[k].head_tag >= 0) kai_eq_heads[entries[k].head_tag] = 1;
    }
}

/* The `Eq.eq` impl of a head, or NULL when its type has none. */
KAI_RT_SHARED_INLINE void *kai_eq_impl_of(int32_t head_tag) {
    if (head_tag <= 0 || head_tag >= kai_eq_heads_len || !kai_eq_heads[head_tag]) return NULL;
    return kai_lookup_impl(KAI_PROTO_EQ, KAI_OP_EQ_EQ, head_tag);
}

typedef struct KaiValue KaiValue;
typedef struct KaiSegment KaiSegment;

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
    union {
        int      b;                                 /* KAI_BOOL */
        int64_t  i;                                 /* KAI_INT */
        double   r;                                 /* KAI_REAL */
        uint32_t c;                                 /* KAI_CHAR */
        uint8_t  byte_val;                                /* KAI_BYTE — Lane 4 (#473) */
        int32_t  i32;                               /* KAI_INT32 */
        uint32_t u32;                               /* KAI_UINT32 */
        uint64_t u64;                               /* KAI_UINT64 */
        /* KAI_INT128. Stored as two 8-byte halves, NOT a bare `__int128`:
         * `__int128` carries a 16-byte alignment requirement that would
         * raise KaiValue's alignment to 16, but the allocator/arena align
         * to 8 — a misaligned `->as.i128` access is UB that segfaults on
         * Linux/-O2 (and UBSan flags it). Access via kai_i128_load /
         * kai_i128_store (memcpy, alignment-agnostic). */
        uint64_t i128_halves[2];                    /* KAI_INT128 (lo, hi) */
        struct { size_t len; char *bytes; } s;      /* KAI_STR (heap, not NUL-terminated but we always allocate +1 byte for safety) */
        struct { KaiValue *head; KaiValue *tail; } cons;
        /* KAI_RANGE: unmaterialised Int list. Inclusive bounds; step is
         * never 0. Empty when step > 0 && from > to (or the mirror for
         * step < 0). */
        struct { int64_t from; int64_t to; int64_t step; } range;
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
            /* A closure's only capture, held in the cell: `captures`
             * then points here, and the closure owns no block. */
            KaiValue   *lone;
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
        /* KAI_VEC: pure value vector. `data` is ONE heap block laid out
         * as [KaiVecMeta][cap * stride bytes of elements]. The meta
         * records how elements are stored (boxed pointers, raw scalars,
         * or inline all-scalar records); len/cap live here so reads
         * never touch the block for bounds. Writes go through the
         * rc==1 uniqueness check: in place when unique, copy-on-write
         * when shared — aliasing is never observable.
         *
         * A SLICE is a KAI_VEC whose `view_of` points at the owning
         * vec node (never at another view — chains flatten on
         * creation). For a view, `data` points at the view's FIRST
         * ELEMENT inside the owner's block (not at the block start),
         * `cap == len`, and the node holds one strong ref on the
         * owner: a live slice pins the whole buffer, and the owner's
         * rc > 1 disables its in-place writes until the slice dies.
         * A view never owns element refs — the owner's free path
         * releases them. Every write through a view copies first
         * (kai_vec_ensure_unique treats any view as shared). */
        struct {
            int64_t     len;
            int64_t     cap;
            void       *data;
            KaiValue   *view_of;
        } vec;
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
        /* KAI_PID: `pid.mb` is `mb`. `owner` is the exit record of the
         * fiber that owns the mailbox, one reference per box, so links
         * and monitors find that fiber even after the mailbox closed. */
        struct {
            struct KaiMailbox   *mb;
            struct KaiFiberExit *owner;
        } pid;
        /* FFI v2 (#417): opaque C handle. The parked pointer is borrowed
         * external memory — the box's RC frees only the KaiValue, never
         * `foreign_ptr` (the driver calls the C destructor itself). */
        void *foreign_ptr;
        /* KAI_CONT: the suspended segment, NULL once resumed or discontinued. */
        KaiSegment *seg;
        /* KAI_SEQ: the thunk or the step, by the state in `variant_tag`. */
        struct { KaiValue *slot; KaiValue *pred; } seq;
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

/* ---------- Vec[T] element-storage metadata ---------- */

/* How a KAI_VEC stores its elements. Decided once, from the first
 * element the vector ever holds; a vector's element type is static so
 * every later element has the same shape. */
#define KAI_VEC_EK_PENDING 0   /* empty vector, no element seen yet */
#define KAI_VEC_EK_BOXED   1   /* KaiValue* per element, each slot RC'd */
#define KAI_VEC_EK_RAW     2   /* one 8-byte scalar payload per element */
#define KAI_VEC_EK_REC     3   /* all-scalar record stored inline */

/* Inline-record field cap: a record with more fields (or any non-scalar
 * field) falls back to boxed storage, which is always sound. */
#define KAI_VEC_REC_MAX 8

/* Prefix of the single heap block behind a KAI_VEC: the block is
 * [KaiVecMeta][cap * stride bytes]. Field names are the static string
 * literals record constructors carry; the meta stores copies of the
 * pointers, never owns the strings. */
typedef struct {
    int32_t     ekind;
    int32_t     elem_tag;                     /* RAW: KaiTag reboxed on read */
    int64_t     stride;                       /* bytes per element */
    int32_t     n_fields;                     /* REC */
    int32_t     head_tag;                     /* REC: nominal head-type tag */
    uint8_t     field_tags[KAI_VEC_REC_MAX];  /* REC: per-field scalar KaiTag */
    const char *names[KAI_VEC_REC_MAX];       /* REC: field names (static) */
} KaiVecMeta;

/* Owner: `data` is the block start ([meta][elems]). View: `data` is
 * the view's first element; the meta lives at the owner's block. Both
 * accessors are the ONLY sanctioned way to reach a vec's meta/elements
 * — direct `as.vec.data` arithmetic breaks on views. */
static inline KaiVecMeta *kai_vec_meta(KaiValue *v) {
    return (KaiVecMeta *) (v->as.vec.view_of ? v->as.vec.view_of->as.vec.data
                                             : v->as.vec.data);
}
static inline char *kai_vec_elems(KaiValue *v) {
    return v->as.vec.view_of ? (char *) v->as.vec.data
                             : (char *) v->as.vec.data + sizeof(KaiVecMeta);
}

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

/* A `Handle` (an aligned LLVM C-API object pointer, owned by its LLVM
 * context, never by RC) rides a boxed slot as an immediate: dup/drop on
 * it are no-ops, and unboxing clears the bit. */
static inline KaiValue *kai_handle_box(void *h) {
    return (KaiValue *) ((uintptr_t) h | (uintptr_t) KAI_INT_TAG_BIT);
}
static inline void *kai_handle_unbox(KaiValue *v) {
    return (void *) ((uintptr_t) v & ~(uintptr_t) KAI_INT_TAG_BIT);
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
KAI_RT_SHARED_INLINE int32_t kai_head_tag(KaiValue *v) {
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
        case KAI_RANGE:   return KAI_HEAD_LIST;
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
        case KAI_VEC:     return KAI_HEAD_VEC;
        case KAI_REF:     return KAI_HEAD_ANON;  /* Ref is not protocol-dispatchable */
        case KAI_FIBER:   return KAI_HEAD_FIBER;
        case KAI_PID:     return KAI_HEAD_PID;
        case KAI_FOREIGN: return KAI_HEAD_ANON;  /* opaque handle is not protocol-dispatchable (#417) */
        case KAI_CONT:    return KAI_HEAD_ANON;
        case KAI_SEQ:     return KAI_HEAD_ANON;
        case KAI_BYTE:    return KAI_HEAD_BYTE;
        /* Fixed-width integers dispatch protocols at compile time (the
         * typer resolves Show/Eq/Ord against the static type), so the
         * runtime head tag is only consulted for a dynamically-typed
         * box — there they ride the Int head, which is sound because no
         * primitive carries a conflicting custom impl. */
        case KAI_INT32:
        case KAI_UINT32:
        case KAI_UINT64:
        case KAI_INT128:  return KAI_HEAD_INT;
    }
    return KAI_HEAD_ANON;
}

/* ---------- allocation and refcounting ---------- */

/* Forward declarations used across sections. */
static int       kai_op_truthy(KaiValue *v);

/* Runtime tracing counters are written unconditionally (only the exit report
 * is env-gated), and are READ across functions (the report, live-peak
 * tracking). Under separate compilation the native-modular merge would inline
 * counter-touching helpers into partitions, giving each its own copy —
 * fragmenting the telemetry KAI_TRACE_RC prints. So they are ONE shared
 * instance (external, owner-defined) under sep-comp, exactly like the runtime's
 * other cross-function state. `KAI_RT_COUNTER(decl, init)` expands to the right
 * linkage: extern everywhere + defined by the owner TU under sep-comp, else the
 * self-contained `static ... = init`. */
/* The counters are Class A (per-thread ledgers, summed at exit), so every
 * form carries KAI_TLS — a _Thread_local extern resolves per thread to the
 * owner's per-thread instance. Under one thread this is byte-identical. */
#if defined(KAI_SEPARATE_COMPILATION)
#  if defined(KAI_RUNTIME_OWNER)
#    define KAI_RT_COUNTER(decl, init) extern KAI_TLS decl; KAI_TLS decl = init
#  else
#    define KAI_RT_COUNTER(decl, init) extern KAI_TLS decl
#  endif
#else
#  define KAI_RT_COUNTER(decl, init) static KAI_TLS decl = init
#endif

#if defined(KAI_SEPARATE_COMPILATION)
#  if defined(KAI_RUNTIME_OWNER)
#    define KAI_RT_ATOMIC_COUNTER(n) extern _Atomic int64_t n; _Atomic int64_t n = 0
#  else
#    define KAI_RT_ATOMIC_COUNTER(n) extern _Atomic int64_t n
#  endif
#else
#  define KAI_RT_ATOMIC_COUNTER(n) static _Atomic int64_t n = 0
#endif
#define KAI_CTR_INC(v) atomic_fetch_add_explicit(&(v), 1, memory_order_relaxed)

/* The status the program chose (main's result or an explicit exit), plus one;
 * 0 while the run has not chosen one, so a runtime abort reads as such. */
KAI_RT_ATOMIC_COUNTER(kai_chosen_exit_plus1);

/* Refcount tracing (m5 #0): always-compiled counters; the per-process
   report at exit is gated on the env var KAI_TRACE_RC. The counters
   add 4 increments per kai_alloc and 2 per kai_free_value — cheap
   enough that the always-on path costs ~ns per allocation, but small
   enough that we keep them on rather than ifdef'ing them in/out
   (otherwise a measurement run would need a runtime rebuild). */
/* The per-cell counters. While the process has one thread they live in a
 * plain global, which the hot path updates inline; a thread-local costs a TLV
 * lookup per access on Mach-O. Before the runtime starts a second thread the
 * global moves into the starting thread's block and every thread counts in
 * its own. The ledger reads only the thread-local blocks, so a report settles
 * the global into them first. */
typedef struct {
    int64_t alloc_total, free_total, live_now, live_peak, reuse_total;
    int64_t alloc_by_tag[16];
} KaiRcHot;
KAI_RT_COUNTER(KaiRcHot kai_rc_hot, {0});
/* C emitted by an older compiler (the bootstrap hop) bumps this name directly. */
KAI_RT_COUNTER(int64_t kai_rc_reuse_total, 0);
#if defined(KAI_SEPARATE_COMPILATION)
extern KaiRcHot kai_rc_hot_st;
extern int kai_rc_mt;
#  if defined(KAI_RUNTIME_OWNER)
KaiRcHot kai_rc_hot_st;
int kai_rc_mt;
#  endif
#else
static KaiRcHot kai_rc_hot_st;
static int kai_rc_mt;
#endif
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
KAI_RT_COUNTER(int64_t kai_arena_alloc_total, 0);
KAI_RT_COUNTER(int64_t kai_arena_free_total, 0);
/* Mailboxes are malloc'd outside the RC ledger; a scope that ends without
 * closing its mailbox shows as mailbox_live != 0 at exit. */
KAI_RT_COUNTER(int64_t kai_mbox_alloc_total, 0);
KAI_RT_COUNTER(int64_t kai_mbox_free_total, 0);
static __attribute__((noinline)) void kai_mbox_count_alloc(void) { kai_mbox_alloc_total++; }
static __attribute__((noinline)) void kai_mbox_count_free(void) { kai_mbox_free_total++; }
/* Vec uniqueness counters: writes that mutated a unique (rc == 1)
 * buffer in place vs. writes that had to copy a shared one. */
KAI_RT_COUNTER(int64_t kai_vec_inplace_total, 0);
KAI_RT_COUNTER(int64_t kai_vec_cow_total, 0);

/* The RC/vec trace ledgers are per-thread and summed at exit. A stale slot
 * mis-attributes counts between threads — wrong telemetry, not corruption —
 * but the address resolves exactly as the allocator slots do, so every
 * thread-local update runs inside a noinline activation, never spilled
 * across a park. */
static inline void kai_rc_hot_alloc(KaiRcHot *h, int tag) {
    h->alloc_total++;
    if (++h->live_now > h->live_peak) h->live_peak = h->live_now;
    if (tag >= 0 && tag < 16) h->alloc_by_tag[tag]++;
}
static inline void kai_rc_hot_live_inc(KaiRcHot *h) {
    if (++h->live_now > h->live_peak) h->live_peak = h->live_now;
}

__attribute__((noinline)) KAI_RT_SHARED void kai_rc_tls_alloc(int tag) { kai_rc_hot_alloc(&kai_rc_hot, tag); }
__attribute__((noinline)) KAI_RT_SHARED void kai_rc_tls_free(void) { kai_rc_hot.free_total++; kai_rc_hot.live_now--; }
__attribute__((noinline)) static void kai_rc_tls_live_inc(void) { kai_rc_hot_live_inc(&kai_rc_hot); }
__attribute__((noinline)) static void kai_rc_tls_live_sub(int64_t n) { kai_rc_hot.live_now -= n; }
__attribute__((noinline)) KAI_RT_SHARED void kai_rc_tls_reuse(void) { kai_rc_hot.reuse_total++; }

/* Called on the only running thread, before it starts a second one. */
__attribute__((noinline)) static void kai_rc_go_mt(void) {
    if (kai_rc_mt) return;
    kai_rc_hot = kai_rc_hot_st;
    kai_rc_mt = 1;
}

__attribute__((noinline)) static void kai_rc_settle(void) {
    if (!kai_rc_mt) kai_rc_hot = kai_rc_hot_st;
}

#if defined(KAI_SEPARATE_COMPILATION)
extern int kai_rc_traffic_on;
#  if defined(KAI_RUNTIME_OWNER)
int kai_rc_traffic_on;
#  endif
#else
static int kai_rc_traffic_on;
#endif
#if !defined(KAI_SEPARATE_COMPILATION) || defined(KAI_RUNTIME_OWNER)
__attribute__((constructor(101))) static void kai_rc_traffic_init(void) {
    kai_rc_traffic_on = getenv("KAI_TRACE_RC") != NULL;
}
#endif
/* The ledger counters are read only by the exit reports, which run under
 * KAI_TRACE_RC; outside a trace build they cost nothing unless that
 * variable is set at startup. */
#ifdef KAI_TRACE_RC
#  define KAI_RC_LEDGER_ON() 1
#else
#  define KAI_RC_LEDGER_ON() __builtin_expect(kai_rc_traffic_on, 0)
#endif
static inline void kai_rc_count_alloc(int tag) {
    if (!KAI_RC_LEDGER_ON()) return;
    if (kai_rc_mt) kai_rc_tls_alloc(tag); else kai_rc_hot_alloc(&kai_rc_hot_st, tag);
}
static inline void kai_rc_count_free(void) {
    if (!KAI_RC_LEDGER_ON()) return;
    if (kai_rc_mt) kai_rc_tls_free();
    else { kai_rc_hot_st.free_total++; kai_rc_hot_st.live_now--; }
}
static inline void kai_rc_count_live_inc(void) {
    if (!KAI_RC_LEDGER_ON()) return;
    if (kai_rc_mt) kai_rc_tls_live_inc(); else kai_rc_hot_live_inc(&kai_rc_hot_st);
}
static inline void kai_rc_count_live_sub(int64_t n) {
    if (!KAI_RC_LEDGER_ON()) return;
    if (kai_rc_mt) kai_rc_tls_live_sub(n); else kai_rc_hot_st.live_now -= n;
}
static inline void kai_rc_count_reuse(void) {
    if (!KAI_RC_LEDGER_ON()) return;
    if (kai_rc_mt) kai_rc_tls_reuse(); else kai_rc_hot_st.reuse_total++;
}
__attribute__((noinline))
static void kai_vec_count_inplace(void) { kai_vec_inplace_total++; }
__attribute__((noinline))
static void kai_vec_count_cow(void) { kai_vec_cow_total++; }
/* #2 parity probe — reuse-token DISPOSAL counter. Bumped by every
 * kai_reuse_free, i.e. every arm-top token captured (UNIQUE scrutinee
 * shell stolen) that the arm body could NOT donate to an in-frame
 * kai_variant_at and therefore had to free. In the rb-tree this is the
 * Black balance arm: `balance_left(insert_loop(l,...), ..., r)` allocates
 * its rebuilt node inside balance_left's own frame, so the stolen Black
 * cell crosses a function boundary the token cannot. This counter is the
 * exact upper bound on what an interprocedural token-pass would recover —
 * if it is small relative to alloc_total, #2 is not worth the ABI cost. */
KAI_RT_COUNTER(int64_t kai_rc_reuse_free_total, 0);
/* #2 parity probe — kai_drop_reuse_token outcome split. unique = shell
 * handed back (donatable); null_shared = rc>1 so cannot steal; null_mismatch
 * = wrong tag/arity (e.g. RBLeaf scrutinee). Tells whether the wasted fresh
 * allocs are a sharing problem (would need a different fix) or genuinely the
 * inter-frame balance case (token-pass). */
KAI_RT_COUNTER(int64_t kai_rc_tok_unique, 0);
KAI_RT_COUNTER(int64_t kai_rc_tok_null_shared, 0);
KAI_RT_COUNTER(int64_t kai_rc_tok_null_mismatch, 0);
/* incref/decref calls that touch `rc` (pinned/INT32_MAX short-circuits are
 * not counted). Compiled into every binary, so a `kai build` output reports
 * them, but counted only when KAI_TRACE_RC is set at startup: they are one
 * process-wide atomic line, and counting every dup/drop serialises threads
 * on it. */
KAI_RT_ATOMIC_COUNTER(kai_rc_incref_total);
KAI_RT_ATOMIC_COUNTER(kai_rc_decref_total);
#define KAI_RC_TRAFFIC_INC(v) \
    do { if (__builtin_expect(kai_rc_traffic_on, 0)) KAI_CTR_INC(v); } while (0)

#ifdef KAI_TRACE_RC
KAI_RT_COUNTER(int64_t kai_rc_free_by_tag[16], {0});
#endif

