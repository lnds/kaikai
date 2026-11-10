/* Fragment of runtime.h; include runtime.h, never this file. */
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Error.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <llvm-c/Linker.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/Support.h>
#include <llvm-c/DebugInfo.h>
#include <stdlib.h>

/* The native backend keeps one context + builder per compilation unit.
 * `kai_llvm_module_new` creates them; the module owns the context for
 * the backend's lifetime (disposed by the process exit, like the
 * out-of-process backends). A handle crosses to kaikai as a raw
 * `void *` (`TyHandle`, never boxed, never RC). */
/* The context and the emit pool are process-global: a partition queued from
 * one translation unit is drained by the exit taken in another. Under
 * KAI_SEPARATE_COMPILATION each therefore gets one shared copy; a per-TU
 * copy drains an empty queue and the process exits before the object is
 * written. The copy is defined by the KAI_RUNTIME_OWNER TU, or by the one
 * that declares itself KAI_LLVM_STATE_OWNER when the runtime owner is
 * compiled without this block. */
#if defined(KAI_SEPARATE_COMPILATION)
#  define KAI_LLVM_STATE
extern LLVMContextRef kai_llvm_ctx;
#else
#  define KAI_LLVM_STATE static
#endif
#if !defined(KAI_SEPARATE_COMPILATION) || defined(KAI_RUNTIME_OWNER) || defined(KAI_LLVM_STATE_OWNER)
#  define KAI_LLVM_STATE_HERE 1
#else
#  define KAI_LLVM_STATE_HERE 0
#endif
#if KAI_LLVM_STATE_HERE
KAI_LLVM_STATE LLVMContextRef kai_llvm_ctx = NULL;
#endif

/* The runtime bitcode a modular partition merges is parsed on a thread started
 * at process load, while the front end runs, into a context the first
 * partition module then adopts. State: 0 none, 1 parsing or parsed, 2 adopted. */
#if defined(KAI_SEPARATE_COMPILATION)
extern LLVMContextRef kai_spare_ctx;
extern LLVMModuleRef kai_spare_mod;
extern pthread_t kai_spare_th;
extern int kai_spare_state;
#endif
#if KAI_LLVM_STATE_HERE
KAI_LLVM_STATE LLVMContextRef kai_spare_ctx = NULL;
KAI_LLVM_STATE LLVMModuleRef kai_spare_mod = NULL;
KAI_LLVM_STATE pthread_t kai_spare_th;
KAI_LLVM_STATE int kai_spare_state = 0;
#endif

static LLVMContextRef kai_llvm_spare_claim(void) {
    pthread_join(kai_spare_th, NULL);
    kai_spare_state = 2;
    return kai_spare_ctx;
}

/* Create a fresh context + module named `name`. Returns the module
 * handle. `name` is a kaikai `String` (a boxed `KaiValue *`); read its
 * bytes with `->as.s.bytes` (the canonical String payload accessor). */
static void *kai_llvm_module_new(KaiValue *name) {
    kai_llvm_ctx = kai_spare_state == 1 ? kai_llvm_spare_claim() : LLVMContextCreate();
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
    void *m, *b, *ptrt, *i64t, *i32t, *i128t, *voidt, *f64t, *fnval;
    KaiNReg *regs; int nregs, regcap;
    KaiNBlk *blks; int nblks, blkcap;
    KaiNFrame *frames; int nframes, framecap;
    int ok;
    int in_fn;   /* begin_fn/end_fn nesting guard (fail loud, not corrupt) */
    void *pad;   /* the landing block a call unwinds to; NULL for none */
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
    c->i128t = LLVMInt128TypeInContext(ctx);
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
            for (int j = 0; r < 0 && j < c->frames[i].nslots; j++)
                if (kai_eff_text_match(c->frames[i].slots[j], eff)) r = (int64_t) j;
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
static void *kai_native_ctx_i128t(void *c) { return ((KaiNativeCtx *) c)->i128t; }
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
    if (c->in_fn) { kai_eprintf("kai: native begin_fn nested (compiler bug)\n"); c->ok = 0; return kai_unit(); }
    for (int i = 0; i < c->nregs; i++) free(c->regs[i].name);
    for (int i = 0; i < c->nblks; i++) free(c->blks[i].label);
    c->nregs = 0; c->nblks = 0; c->fnval = fnval; c->in_fn = 1; c->pad = NULL;
    /* DWARF (#500): start each fn with NO subprogram + no current location.
     * Only a fn that calls `native_di_subprogram` (the user-fn walk) gets a
     * scope; a synthetic fn (thunk / runtime hook / runner driver) keeps
     * `disub == NULL`, so its instructions carry no `!dbg` — which avoids a
     * location whose scope is the PREVIOUS fn's subprogram (the verifier's
     * "wrong subprogram" rejection). The builder's stale location is
     * cleared too so no instruction inherits one across the fn boundary. */
    c->disub = NULL;
    if (c->b) LLVMSetCurrentDebugLocation2((LLVMBuilderRef) c->b, NULL);
    return kai_unit();
}
static KaiValue *kai_native_ctx_end_fn(void *cv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    c->in_fn = 0; c->pad = NULL;
    return kai_unit();
}

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
/* A raw i128 constant from its decimal text. Int128 literals may exceed the
 * compiler's 64-bit `decode_int`, so the register const is materialised from
 * the textual span (base 10, optional leading `-`) — the 128-bit value LLVM
 * cannot take through the i64 `const_int`. */
/* The span is the literal verbatim, so it may carry a `0x`/`0b` base
 * prefix and `_` separators. LLVM parses neither: strip both and pass
 * the matching radix, else the digits are read as decimal and silently
 * yield a wrong constant. */
static void *kai_llvm_const_i128_str(void *i128ty, KaiValue *s) {
    const char *txt = s->as.s.bytes;
    size_t len = s->as.s.len;
    char buf[64];
    size_t i = 0, n = 0;
    unsigned radix = 10;
    if (i < len && (txt[i] == '-' || txt[i] == '+')) {
        if (txt[i] == '-' && n < sizeof buf - 1) buf[n++] = '-';
        i++;
    }
    if (i + 1 < len && txt[i] == '0') {
        char b = txt[i + 1];
        if (b == 'x' || b == 'X') { radix = 16; i += 2; }
        else if (b == 'b' || b == 'B') { radix = 2; i += 2; }
    }
    for (; i < len && n < sizeof buf - 1; i++) {
        if (txt[i] != '_') buf[n++] = txt[i];
    }
    void *r = (void *) LLVMConstIntOfStringAndSize((LLVMTypeRef) i128ty, buf, (unsigned) n, radix);
    kai_decref(s);
    return r;
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
/* `sret(<struct>)` on the hidden first parameter of a struct-returning
 * extern — the ABI directive that makes a MEMORY-class return write
 * through a caller-supplied pointer. Applied to the declaration's param 0
 * and the matching call-site arg 0. */
static void kai_llvm_sret_attr_at(LLVMModuleRef m, LLVMValueRef fn_or_call,
                                  int is_call, LLVMTypeRef sty) {
    LLVMContextRef ctx = LLVMGetModuleContext(m);
    unsigned kind = LLVMGetEnumAttributeKindForName("sret", 4);
    LLVMAttributeRef a = LLVMCreateTypeAttribute(ctx, kind, sty);
    if (is_call) LLVMAddCallSiteAttribute(fn_or_call, 1u, a);
    else LLVMAddAttributeAtIndex(fn_or_call, 1u, a);
}
static KaiValue *kai_llvm_add_sret_decl(void *m, void *fn, void *sty) {
    kai_llvm_sret_attr_at((LLVMModuleRef) m, (LLVMValueRef) fn, 0, (LLVMTypeRef) sty);
    return kai_unit();
}
static KaiValue *kai_llvm_add_sret_call(void *m, void *call, void *sty) {
    kai_llvm_sret_attr_at((LLVMModuleRef) m, (LLVMValueRef) call, 1, (LLVMTypeRef) sty);
    return kai_unit();
}
/* Function attributes, attached at `LLVMAttributeFunctionIndex` on the
 * declaration so every call site the optimizer sees carries them.
 * `memory(none)` takes the MemoryEffects bitmask, and `none()` is 0. A trap
 * unwinds, so a fn that can trap must not be `nounwind`; and `willreturn` is
 * never stamped, or a memory(none) fn that traps could be deleted. */
static void kai_llvm_add_enum_fn_attr(LLVMValueRef fn, const char *name,
                                      unsigned name_len, uint64_t val) {
    LLVMContextRef ctx = LLVMGetTypeContext(LLVMTypeOf(fn));
    unsigned kind = LLVMGetEnumAttributeKindForName(name, name_len);
    LLVMAttributeRef a = LLVMCreateEnumAttribute(ctx, kind, val);
    LLVMAddAttributeAtIndex((LLVMValueRef) fn, LLVMAttributeFunctionIndex, a);
}
static KaiValue *kai_llvm_add_nounwind(void *fn) {
    kai_llvm_add_enum_fn_attr((LLVMValueRef) fn, "nounwind", 8, 0);
    return kai_unit();
}
static KaiValue *kai_llvm_add_memory_none(void *fn) {
    kai_llvm_add_enum_fn_attr((LLVMValueRef) fn, "memory", 6, 0);
    return kai_unit();
}
/* Mark a direct call as a guaranteed tail call; 0 when the value is not a
 * call (an `invoke` under a landing pad cannot be one). */
static int64_t kai_llvm_set_musttail(void *call) {
    if (!LLVMIsACallInst((LLVMValueRef) call)) return 0;
    LLVMSetTailCallKind((LLVMValueRef) call, LLVMTailCallKindMustTail);
    return 1;
}
/* Demote a DEFINED function to internal linkage. The modular/split root
 * partitions emit user fns raw (external) so cross-TU stdlib calls link; a
 * root-private user fn keeps a bare source name, which then overrides a libc
 * or runtime symbol the C owner TU calls (issue #1380: a user `fn read`
 * shadowed POSIX `read`). The emitter calls this on those fns so their name
 * stops participating in link-wide resolution. */
static KaiValue *kai_llvm_set_internal_linkage(void *fn) {
    LLVMSetLinkage((LLVMValueRef) fn, LLVMInternalLinkage);
    return kai_unit();
}
/* The wasm flavour a build targets, from `KAI_NATIVE_TARGET`: 0 = the host,
 * 1 = wasm32-wasi, 2 = wasm32-browser. */
static int kai_llvm_wasm_target(void) {
    const char *t = getenv("KAI_NATIVE_TARGET");
    if (!t || !t[0]) return 0;
    if (strcmp(t, "wasm32-wasi") == 0) return 1;
    if (strcmp(t, "wasm32-browser") == 0) return 2;
    return 0;
}

/* The triple the module and the object are built for; dispose with
 * `LLVMDisposeMessage`. */
static char *kai_llvm_target_triple(void) {
    switch (kai_llvm_wasm_target()) {
        case 1: return LLVMCreateMessage("wasm32-unknown-wasip1");
        case 2: return LLVMCreateMessage("wasm32-unknown-unknown");
        default: return LLVMGetDefaultTargetTriple();
    }
}

/* The target C-ABI class of the native backend's default triple: 1 =
 * AArch64 (AAPCS64), 2 = x86-64 SysV, 0 = anything else (the emitter keeps
 * the honest struct-by-value reject there). Read once at module build; the
 * object emission below pins the SAME `LLVMGetDefaultTargetTriple()`. */
static int64_t kai_native_target_abi(void) {
    char *triple = kai_llvm_target_triple();
    int64_t plat = 0;
    if (triple) {
        if (strncmp(triple, "arm64", 5) == 0 || strncmp(triple, "aarch64", 7) == 0) plat = 1;
        else if (strncmp(triple, "x86_64", 6) == 0) {
            /* SysV only — a `x86_64-pc-windows` triple uses a different
             * struct ABI the classifier does not model. */
            if (strstr(triple, "windows") == NULL) plat = 2;
        }
        LLVMDisposeMessage(triple);
    }
    return plat;
}
/* Identity of everything OUTSIDE the KIR that shapes an emitted native
 * object: the target triple, the opt level, and the runtime bitcodes the
 * emit step merges. Folded into the cached core object's content key so a
 * toolchain-, target- or bitcode-level change can never resurrect a stale
 * object whose KIR text happens to match. Empty string when the split
 * cache must not engage: an explicit KAI_NATIVE_CORE_OBJ=0, a
 * non-default opt level (`--debug` keeps the single-object path and its
 * dSYM flow), or the whole-program runtime bitcode present without its
 * inline twin — a split partition would then leave every `kaix_*` op an
 * out-of-line call (an order of magnitude on hot loops), while the
 * whole-program merge still inlines them. Bitcode identity is mtime-size
 * (the kaic2 toolchain-id discipline), not a content hash: a regenerated
 * identical bitcode costs one spurious miss, never a stale hit. */
static void kai_llvm_bc_id(const char *env, char *out, size_t outsz) {
    const char *p = getenv(env);
    struct stat st;
    if (p && p[0] && stat(p, &st) == 0)
        snprintf(out, outsz, "%lld-%lld", (long long) st.st_mtime, (long long) st.st_size);
    else
        snprintf(out, outsz, "none");
}

/* Codegen level for the TargetMachine — instruction selection, scheduling
 * and register allocation, a budget separate from the IR pass pipeline.
 *
 * `Default` for every build profile. `None` emits correct code ~4x faster
 * but spends the register allocator's budget: on the rb-tree descent it
 * quadruples the spill traffic in the hot loop (349 -> 1508 load/store)
 * and doubles the program's retired instructions. Emitted-code speed
 * outranks compile time here.
 *
 * `KAI_NATIVE_CGLEVEL=0` selects the fast level, opt-in and never by
 * default. Note it reaches only the prebuilt core object, not the user's
 * own module — so it was never a usable escape from the default this
 * replaces, and `--release` was not one either. It rides the backend tag
 * that keys the shared core-object cache, so a fast-emit object cannot be
 * served to a build that did not ask for one. */
static const char *kai_llvm_cgen_level_id(void) {
    const char *e = getenv("KAI_NATIVE_CGLEVEL");
    if (e && e[0]) return (strcmp(e, "0") == 0) ? "none" : "default";
    return "default";
}

static LLVMCodeGenOptLevel kai_llvm_cgen_level(void) {
    return (strcmp(kai_llvm_cgen_level_id(), "none") == 0)
        ? LLVMCodeGenLevelNone : LLVMCodeGenLevelDefault;
}
static KaiValue *kai_llvm_backend_tag(void) {
#ifndef KAI_LLVM_WEBASSEMBLY
    if (kai_llvm_wasm_target()) return kai_str("");
#endif
    const char *off = getenv("KAI_NATIVE_CORE_OBJ");
    if (off && strcmp(off, "0") == 0) return kai_str("");
    /* A debug build's DWARF names the program's own source file, so its core
     * object is never program-invariant: no tag, no shared core object. */
    const char *mode = getenv("KAI_BUILD_MODE");
    if (mode && strcmp(mode, "debug") == 0) return kai_str("");
    /* The opt level keys the object: each level gets its own shared core
     * object, so `kai run` (1) and `kai test` (0) reuse one too. */
    const char *lvl = getenv("KAI_NATIVE_OPT");
    if (!lvl || !lvl[0]) lvl = "2";
    char bc[64], inlbc[64], tag[512];
    kai_llvm_bc_id("KAI_NATIVE_RUNTIME_BC", bc, sizeof bc);
    kai_llvm_bc_id("KAI_NATIVE_RUNTIME_INLINE_BC", inlbc, sizeof inlbc);
    if (strcmp(bc, "none") != 0 && strcmp(inlbc, "none") == 0) {
        static int noted = 0;
        if (!noted) {
            noted = 1;
            kai_eprintf("kai: native runtime inline bitcode unavailable — "
                    "skipping the core-object split, using the whole-program "
                    "runtime merge (slower build, runtime ops stay inlined)\n");
        }
        return kai_str("");
    }
    char *triple = kai_llvm_target_triple();
    snprintf(tag, sizeof tag, "%s|O%s|cg:%s|bc:%s|inlbc:%s",
             triple ? triple : "?", lvl, kai_llvm_cgen_level_id(), bc, inlbc);
    if (triple) LLVMDisposeMessage(triple);
    return kai_str(tag);
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

/* --- table-driven unwinding ---
 * While a landing block of the function being built is set,
 * `native_build_call` emits `invoke` to it and continues in a fresh
 * block; otherwise it emits a plain call. A function emitted while
 * another is open (a thunk, an FFI wrapper) never reaches its pad. */
static KaiValue *kai_native_ctx_set_pad(void *cv, void *bb) {
    ((KaiNativeCtx *) cv)->pad = bb;
    return kai_unit();
}
static void *kai_native_ctx_pad(void *cv) { return ((KaiNativeCtx *) cv)->pad; }
static void *kai_llvm_get_insert_block(void *b) {
    return (void *) LLVMGetInsertBlock((LLVMBuilderRef) b);
}
static void *kai_native_build_call(void *cv, void *fn, void *fnty, void *buf) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    LLVMBuilderRef b = (LLVMBuilderRef) c->b;
    LLVMBasicBlockRef here = LLVMGetInsertBlock(b);
    if (!c->pad || LLVMGetBasicBlockParent((LLVMBasicBlockRef) c->pad) != LLVMGetBasicBlockParent(here))
        return (void *) LLVMBuildCall2(b, (LLVMTypeRef) fnty, (LLVMValueRef) fn,
                                       (LLVMValueRef *) bf->xs, (unsigned) bf->n, "");
    LLVMBasicBlockRef next = LLVMAppendBasicBlockInContext(
        LLVMGetModuleContext((LLVMModuleRef) c->m), LLVMGetBasicBlockParent(here), "");
    LLVMMoveBasicBlockAfter(next, here);
    LLVMValueRef r = LLVMBuildInvoke2(b, (LLVMTypeRef) fnty, (LLVMValueRef) fn,
                                      (LLVMValueRef *) bf->xs, (unsigned) bf->n,
                                      next, (LLVMBasicBlockRef) c->pad, "");
    LLVMPositionBuilderAtEnd(b, next);
    return (void *) r;
}
/* `invoke` continues at `then_bb` on return and at `unwind_bb` when an
 * unwind passes through the call; the landing pad there is a cleanup that
 * `resume`s the unwind once it has released the frame's references. */
static void *kai_llvm_build_invoke_n(void *b, void *fn, void *fnty, void *buf,
                                     void *then_bb, void *unwind_bb) {
    KaiLlvmBuf *bf = (KaiLlvmBuf *) buf;
    return (void *) LLVMBuildInvoke2((LLVMBuilderRef) b, (LLVMTypeRef) fnty,
                                     (LLVMValueRef) fn, (LLVMValueRef *) bf->xs,
                                     (unsigned) bf->n, (LLVMBasicBlockRef) then_bb,
                                     (LLVMBasicBlockRef) unwind_bb, "");
}
static void *kai_llvm_build_landingpad_cleanup(void *b) {
    LLVMBasicBlockRef bb = LLVMGetInsertBlock((LLVMBuilderRef) b);
    LLVMContextRef ctx = LLVMGetTypeContext(LLVMTypeOf(LLVMGetBasicBlockParent(bb)));
    LLVMTypeRef fields[2] = { LLVMPointerTypeInContext(ctx, 0), LLVMInt32TypeInContext(ctx) };
    LLVMTypeRef ty = LLVMStructTypeInContext(ctx, fields, 2, 0);
    LLVMValueRef lp = LLVMBuildLandingPad((LLVMBuilderRef) b, ty, NULL, 0, "");
    LLVMSetCleanup(lp, 1);
    return (void *) lp;
}
static KaiValue *kai_llvm_build_resume(void *b, void *lp) {
    LLVMBuildResume((LLVMBuilderRef) b, (LLVMValueRef) lp);
    return kai_unit();
}
static KaiValue *kai_llvm_set_personality(void *fn, void *pers) {
    LLVMSetPersonalityFn((LLVMValueRef) fn, (LLVMValueRef) pers);
    return kai_unit();
}
/* Asynchronous unwind tables (UWTableKind::Async = 2), so an unwind can
 * walk through the function from any of its calls. */
static KaiValue *kai_llvm_add_uwtable(void *fn) {
    kai_llvm_add_enum_fn_attr((LLVMValueRef) fn, "uwtable", 7, 2);
    return kai_unit();
}

/* --- function lookup / declaration ---
 * Resolve a function by name; declare it (no body) if absent. The walk
 * uses this for runtime externs (`kaix_*`) and forward references to
 * user fns the module defines later. `LLVMGetNamedFunction` returns NULL
 * when absent, so a miss adds the declaration with the given type. */
static void *kai_llvm_get_or_declare_fn(void *m, KaiValue *name, void *fnty) {
    LLVMValueRef fn = LLVMGetNamedFunction((LLVMModuleRef) m, name->as.s.bytes);
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

