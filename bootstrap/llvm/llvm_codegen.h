/* Fragment of runtime.h; include runtime.h, never this file. */
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
    /* The insert block's function: `c->fnval` can name another function
     * than the one being built. */
    LLVMBasicBlockRef entry = LLVMGetEntryBasicBlock(LLVMGetBasicBlockParent(cur));
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
    LLVMValueRef s = LLVMBuildStore((LLVMBuilderRef) b, (LLVMValueRef) val, (LLVMValueRef) ptr);
    /* Stamp the natural alignment from the value's type. Without a module
     * DataLayout at emit time LLVM infers the ABI-generic (preferred) alignment,
     * which is 4 for i64 — a store i64 align 4 the x86-64 -O2 backend lowers to a
     * misaligned access that SIGSEGVs (arm64/-O0 tolerates it, hiding the bug).
     * An `[n x i64]` slot is 8-aligned; stamp it so the store agrees. */
    LLVMTypeRef vt = LLVMTypeOf((LLVMValueRef) val);
    LLVMTypeKind k = LLVMGetTypeKind(vt);
    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth(vt);
        LLVMSetAlignment(s, bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8);
    } else if (k == LLVMPointerTypeKind || k == LLVMDoubleTypeKind) {
        LLVMSetAlignment(s, 8);
    } else if (k == LLVMFloatTypeKind) {
        LLVMSetAlignment(s, 4);
    }
    return kai_unit();
}
static void *kai_llvm_build_load(void *b, void *ty, void *ptr) {
    LLVMValueRef l = LLVMBuildLoad2((LLVMBuilderRef) b, (LLVMTypeRef) ty, (LLVMValueRef) ptr, "");
    /* Same natural-alignment stamp as `kai_llvm_build_store`: without a module
     * DataLayout at emit time a `load i64` gets ABI-generic align 4, which the
     * x86-64 -O2 backend lowers to a misaligned load that SIGSEGVs. */
    LLVMTypeKind k = LLVMGetTypeKind((LLVMTypeRef) ty);
    if (k == LLVMIntegerTypeKind) {
        unsigned bits = LLVMGetIntTypeWidth((LLVMTypeRef) ty);
        LLVMSetAlignment(l, bits <= 8 ? 1 : bits <= 16 ? 2 : bits <= 32 ? 4 : 8);
    } else if (k == LLVMPointerTypeKind || k == LLVMDoubleTypeKind) {
        LLVMSetAlignment(l, 8);
    } else if (k == LLVMFloatTypeKind) {
        LLVMSetAlignment(l, 4);
    }
    return (void *) l;
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
/* Raw `double` comparison → an `i1`. `pred` selects the predicate:
 * 0=`<`, 1=`>`, 2=`<=`, 3=`>=`, 4=`==`, 5=`!=` — what the C operator
 * gives on a raw `double`: a NaN compares false, except under `!=`,
 * which is the unordered predicate and so the negation of `==`.
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
        default: p = LLVMRealUNE; break;
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
/* Logical NOT of a raw bool (`!x`), width-independent. The raw-Bool
 * register is `i32` 0/1, not `i1`: a bitwise complement there yields
 * `~1 == -2` — still truthy for every consumer. Compare against zero and
 * re-widen so the result is the 0/1 a raw-Bool slot must hold. */
static void *kai_llvm_build_lnot(void *b, void *a) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a;
    LLVMTypeRef ty = LLVMTypeOf(la);
    LLVMValueRef z = LLVMBuildICmp(bld, LLVMIntEQ, la, LLVMConstNull(ty), "");
    return (void *) LLVMBuildZExt(bld, z, ty, "");
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
/* Pointer↔integer round-trip for the i64-inline variant slot buffer: a boxed
 * `KaiValue *` slot enters an `[n x i64]` payload word via `ptrtoint`, and
 * a raw word is read back as a pointer via `inttoptr`. `KaiVarSlot` is a
 * one-word union, so the bits survive the round-trip unchanged. */
static void *kai_llvm_build_ptrtoint(void *b, void *v, void *ty) {
    return (void *) LLVMBuildPtrToInt((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
}
static void *kai_llvm_build_inttoptr(void *b, void *v, void *ty) {
    return (void *) LLVMBuildIntToPtr((LLVMBuilderRef) b, (LLVMValueRef) v, (LLVMTypeRef) ty, "");
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
 * wrapping ops (no-wrap flags unset). Cases 3/4 (`sdiv`/`srem`) remain for
 * completeness but the raw div/mod path no longer routes through them — it
 * calls the checked helper `kai_idiv_chk`/`kai_imod_chk` so div-by-zero and
 * INT64_MIN/-1 trap. Operands + result are `i64`, NOT boxed Ints: no RC, so
 * the use-after-free the boxed path hit on a multi-use raw operand cannot
 * arise. */
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
/* Raw UNSIGNED comparison → an `i1`, for the UInt32/UInt64 raw path.
 * `pred`: 0=`<`, 1=`>`, 2=`<=`, 3=`>=`, 4=`==`, 5=`!=` — the unsigned
 * (`U*`) family (`==`/`!=` are signedness-agnostic). */
static void *kai_llvm_build_ucmp(void *b, int64_t pred, void *a, void *c) {
    LLVMBuilderRef bld = (LLVMBuilderRef) b;
    LLVMValueRef la = (LLVMValueRef) a, lc = (LLVMValueRef) c;
    LLVMIntPredicate p;
    switch (pred) {
        case 0:  p = LLVMIntULT; break;
        case 1:  p = LLVMIntUGT; break;
        case 2:  p = LLVMIntULE; break;
        case 3:  p = LLVMIntUGE; break;
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
/* An `i8*` to a private global string constant. Both entry points store
 * their bytes verbatim: the caller decodes. `kai_llvm_build_global_string`
 * takes a NUL-terminated name; `kai_llvm_build_string_span` honours the
 * KaiValue's length, so a literal carrying an embedded NUL survives.
 * The builder must be positioned in a block; the global is module-level
 * + private. */
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
    /* The literal arrives ALREADY DECODED from the compiler's shared
     * escape table (compiler/escapes.kai), carrying its own length, so
     * an embedded NUL survives. This used to re-decode C99 escapes here
     * to match what `cc` did for the C backend's verbatim
     * `kai_str("...")` — two tables that drifted apart. */
    const char *buf = s->as.s.bytes;
    size_t w = s->as.s.len;
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

/* Modular backend: the default-evidence globals cross object boundaries, so
 * the owner TU defines them EXTERNAL (zero-init, cross-TU visible) and every
 * other TU declares them EXTERNAL with no initializer. Same shape as
 * `add_global_zeroed` but external linkage — the single-TU path keeps
 * internal linkage so its byte-id is untouched. */
static void *kai_llvm_add_global_extern_def(void *m, void *ty, KaiValue *name) {
    LLVMValueRef g = LLVMAddGlobal((LLVMModuleRef) m, (LLVMTypeRef) ty, name->as.s.bytes);
    LLVMSetInitializer(g, LLVMConstNull((LLVMTypeRef) ty));
    LLVMSetLinkage(g, LLVMExternalLinkage);
    if (name) kai_decref(name);
    return (void *) g;
}

/* A leaf TU's external DECLARATION of an owner-defined global: no initializer
 * (LLVM reads that as an external declaration) so the definition stays unique
 * to the owner and the link resolves the reference across objects. */
static void *kai_llvm_add_global_extern_decl(void *m, void *ty, KaiValue *name) {
    LLVMValueRef g = LLVMAddGlobal((LLVMModuleRef) m, (LLVMTypeRef) ty, name->as.s.bytes);
    LLVMSetLinkage(g, LLVMExternalLinkage);
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
 * metadata silently). Idempotent — a second call is a no-op.
 *
 * Gated on KAI_BUILD_MODE=debug, read HERE in C — the same place the opt
 * level is gated (`kai_llvm_pass_pipeline` reads KAI_NATIVE_OPT). The
 * kaikai walk calls this unconditionally; a non-debug build leaves `c->dib`
 * NULL, so every later DI prim is a no-op and the module is byte-identical
 * to before this lane. Keeping the gate in C means the emit_native walk
 * needs no env-var prim. */
static KaiValue *kai_native_di_enable(void *cv, KaiValue *fnamev, KaiValue *dirv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    const char *mode = getenv("KAI_BUILD_MODE");
    if (!mode || strcmp(mode, "debug") != 0) { kai_decref(fnamev); kai_decref(dirv); return kai_unit(); }
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
    /* The kaikai-side path is the entry file kaic2 saw — which `bin/kai`
     * copied to $tmp, so it is ephemeral. KAI_DEBUG_SRC carries the user's
     * ORIGINAL absolute path; prefer it so the DIFile + comp_dir point at the
     * real source `lldb`/`gdb` can open. Split it into (dir, base) here. */
    const char *fname = fnamev->as.s.bytes;
    const char *dir = dirv->as.s.bytes;
    char dirbuf[4096];
    const char *real = getenv("KAI_DEBUG_SRC");
    if (real && real[0]) {
        const char *slash = strrchr(real, '/');
        if (slash) {
            size_t dl = (size_t) (slash - real);
            if (dl >= sizeof dirbuf) dl = sizeof dirbuf - 1;
            memcpy(dirbuf, real, dl); dirbuf[dl] = '\0';
            dir = dirbuf; fname = slash + 1;
        } else {
            fname = real;
        }
    }
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
    /* Seed the builder's current location to this fn's line UNDER THE NEW
     * subprogram, so the prologue (allocas / param store / entry br, all
     * emitted before the first statement's `set_loc`) carries a `!dbg`
     * scoped to THIS fn — not the previous fn's lingering location, which
     * the shared builder would otherwise keep and which the verifier
     * rejects as "wrong subprogram". The builder is still positioned in the
     * previous fn here; this only sets the location state, which persists
     * across the `position_at_end` the caller does next. */
    LLVMContextRef lctx = LLVMGetModuleContext((LLVMModuleRef) c->m);
    LLVMMetadataRef ploc = LLVMDIBuilderCreateDebugLocation(lctx, ln, 0, sp, NULL);
    LLVMSetCurrentDebugLocation2((LLVMBuilderRef) c->b, ploc);
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

/* Emit the strong `__kai_build_debug = 1` global that flips the runtime's
 * weak default (0), so a panic in THIS (debug) binary resolves its backtrace
 * to .kai:line. No-op when DI is off, so a release/default binary keeps the
 * weak 0 and the one-line panic. */
static KaiValue *kai_native_di_debug_marker(void *cv) {
    KaiNativeCtx *c = (KaiNativeCtx *) cv;
    if (!c->dib) return kai_unit();
    LLVMModuleRef m = (LLVMModuleRef) c->m;
    LLVMTypeRef i32 = LLVMInt32TypeInContext(LLVMGetModuleContext(m));
    LLVMValueRef g = LLVMAddGlobal(m, i32, "__kai_build_debug");
    LLVMSetInitializer(g, LLVMConstInt(i32, 1, 0));
    LLVMSetGlobalConstant(g, 1);
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

/* `prune` leads with globaldce: the merged runtime bodies are internal, so the
 * ones the module never reaches go before the pipeline spends work on them. */
static int64_t kai_llvm_run_passes(LLVMModuleRef m, LLVMTargetMachineRef tm, int prune) {
    char pipeline[64];
    snprintf(pipeline, sizeof pipeline, "%s%s", prune ? "globaldce," : "", kai_llvm_pass_pipeline());
    LLVMPassBuilderOptionsRef opts = LLVMCreatePassBuilderOptions();
    LLVMErrorRef e = LLVMRunPasses(m, pipeline, tm, opts);
    LLVMDisposePassBuilderOptions(opts);
    if (e) {
        char *msg = LLVMGetErrorMessage(e);   /* consumes e + allocates msg */
        kai_eprintf("kai: native opt pass pipeline (%s) failed: %s\n",
                pipeline, msg ? msg : "?");
        LLVMDisposeErrorMessage(msg);         /* frees msg; do NOT ConsumeError(e) */
        return 1;
    }
    return 0;
}

