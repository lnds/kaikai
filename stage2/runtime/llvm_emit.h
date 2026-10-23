/* Fragment of runtime.h; include runtime.h, never this file. */
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
/* The program-emitted entry points the runtime OWNER object (cc-compiled
 * runtime_llvm.c) calls into this whole-program object: its `main` invokes
 * these across the object boundary, so they MUST stay external through the
 * internaliser or the final cc link cannot resolve them. Before the hot/owner
 * split `main` lived in the merged bitcode and called `kai_main` intra-module,
 * so internalising all-but-`main` was enough; now `main` is owner-only. */
static int kai_llvm_is_owner_entry_point(const char *nm) {
    if (!nm) return 0;
    return strcmp(nm, "main") == 0                       /* OS entry (owner-defined now) */
        || strcmp(nm, "kai_main") == 0                   /* program body the owner's main runs */
        || strcmp(nm, "_kai_proto_init_llvm") == 0       /* proto table init */
        || strcmp(nm, "kai_main_install_defaults") == 0  /* default-handler install */
        || strcmp(nm, "kai_main_teardown_defaults") == 0;/* default-handler teardown */
}

static void kai_llvm_internalize_except_main(LLVMModuleRef m) {
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;            /* nothing to internalise */
        if (kai_llvm_is_owner_entry_point(LLVMGetValueName(f))) continue;
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

/* Bitcode laid out for another architecture links without complaint once its
 * triple is overwritten, and miscompiles; refuse it. */
static int kai_llvm_bc_arch_clash(LLVMModuleRef src, const char *dst_triple, const char *bc_path) {
    const char *src_triple = LLVMGetTarget(src);
    size_t n = 0;
    while (src_triple && dst_triple && src_triple[n] && src_triple[n] != '-') n++;
    if (!src_triple || !dst_triple || (strncmp(src_triple, dst_triple, n) == 0 && (dst_triple[n] == '-' || dst_triple[n] == 0)))
        return 0;
    kai_eprintf("kai: runtime bitcode %s is for %s, but this build targets %s\n", bc_path, src_triple, dst_triple);
    return 1;
}

static int64_t kai_llvm_link_runtime_bc(void *m) {
    const char *bc_path = getenv("KAI_NATIVE_RUNTIME_BC");
    if (!bc_path || !bc_path[0]) return 0;             /* opt-out: legacy cc-links path */

    LLVMMemoryBufferRef buf = NULL;
    char *err = NULL;
    if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &err)) {
        /* File named but unreadable — not a no-op situation (the driver set
         * the var, so it expected a bitcode). Fail loudly. */
        kai_eprintf("kai: native runtime bitcode unreadable (%s): %s\n",
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
        kai_eprintf("kai: native runtime bitcode parse failed (%s): %s\n",
                bc_path, err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        return 1;
    }
    if (kai_llvm_bc_arch_clash(src, LLVMGetTarget((LLVMModuleRef) m), bc_path)) return 1;

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
        kai_eprintf("kai: native runtime bitcode link failed (%s)\n", bc_path);
        return 1;
    }

    kai_llvm_internalize_except_main((LLVMModuleRef) m);
    return 0;
}

/* Modular runtime-bc merge: link the SEPARATE-COMPILATION runtime bitcode (its
 * state globals are `external`, owned by the one runtime TU at link) into a
 * partition so O2 inlines the `kaix_*` hot ops, WITHOUT duplicating runtime
 * STATE across the N partition objects.
 *
 * The whole-program path internalises everything-but-main, which would strip
 * the external linkage a cross-partition user call depends on. This path
 * relinks only the RUNTIME functions the merge brought in — a function is
 * "from the runtime" iff it was NOT defined in the partition before the
 * merge. The user's own fns keep their linkage, and the runtime's state
 * globals stay `external` references resolved by the runtime owner, so one
 * runtime-state instance survives. Returns 0 on success or no-op (bc path
 * unset), non-zero only on a real parse/link failure. */
/* Exported runtime bodies above this many instructions are never worth
 * inlining; the runtime owner object defines them. */
#define KAI_LLVM_TRIM_MAX_INSNS 30

static int kai_llvm_fn_has_enum_attr(LLVMValueRef f, const char *name) {
    unsigned kind = LLVMGetEnumAttributeKindForName(name, strlen(name));
    return LLVMGetEnumAttributeAtIndex(f, LLVMAttributeFunctionIndex, kind) != NULL;
}

/* Turn a large exported non-`kaix_` function of the runtime bitcode into a
 * declaration, so the partition neither links nor optimises nor emits what it
 * will call through the owner. The `kaix_*` ops, internal helpers (inlined or
 * dropped by globaldce) and small or hinted functions keep their bodies.
 * Returns 1 if a block is still in use when it is deleted. */
static int kai_llvm_trim_runtime_bc(LLVMModuleRef src) {
    for (LLVMValueRef f = LLVMGetFirstFunction(src); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f) || LLVMGetLinkage(f) != LLVMExternalLinkage) continue;
        const char *nm = LLVMGetValueName(f);
        if (!nm || strncmp(nm, "kaix_", 5) == 0) continue;
        if (kai_llvm_fn_has_enum_attr(f, "alwaysinline") || kai_llvm_fn_has_enum_attr(f, "inlinehint")) continue;
        unsigned n = 0;
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb; bb = LLVMGetNextBasicBlock(bb))
            for (LLVMValueRef i = LLVMGetFirstInstruction(bb); i; i = LLVMGetNextInstruction(i)) n++;
        if (n <= KAI_LLVM_TRIM_MAX_INSNS) continue;
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb; bb = LLVMGetNextBasicBlock(bb))
            for (LLVMValueRef i = LLVMGetFirstInstruction(bb); i; i = LLVMGetNextInstruction(i))
                if (LLVMGetTypeKind(LLVMTypeOf(i)) != LLVMVoidTypeKind)
                    LLVMReplaceAllUsesWith(i, LLVMGetUndef(LLVMTypeOf(i)));
        /* Every instruction goes before any block: a terminator still using a
         * deleted block would write to freed memory when it is erased. */
        for (LLVMBasicBlockRef bb = LLVMGetFirstBasicBlock(f); bb; bb = LLVMGetNextBasicBlock(bb)) {
            LLVMValueRef i;
            while ((i = LLVMGetFirstInstruction(bb)) != NULL) LLVMInstructionEraseFromParent(i);
        }
        LLVMBasicBlockRef bb;
        while ((bb = LLVMGetFirstBasicBlock(f)) != NULL) {
            if (LLVMGetFirstUse(LLVMBasicBlockAsValue(bb))) return 1;
            LLVMDeleteBasicBlock(bb);
        }
    }
    return 0;
}

static int64_t kai_llvm_link_runtime_bc_modular(void *m) {
    const char *bc_path = getenv("KAI_NATIVE_RUNTIME_INLINE_BC");
    if (!bc_path || !bc_path[0]) return 0;             /* opt-out: runtime stays an owner-TU call */

    LLVMModuleRef mod = (LLVMModuleRef) m;

    /* Snapshot the partition's OWN function names before the merge, so the
     * post-merge internaliser can tell a runtime body (to fold away) from a
     * user body (to keep external for cross-TU linkage). A partition has a
     * bounded fn count; a flat name set keeps the check simple and allocation
     * a single grow-on-demand array. */
    size_t pre_cap = 64, pre_n = 0;
    char **pre = (char **) malloc(pre_cap * sizeof(char *));
    if (!pre) { kai_eprintf("kai: native modular merge OOM\n"); return 1; }
    for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;
        const char *nm = LLVMGetValueName(f);
        if (!nm) continue;
        if (pre_n == pre_cap) {
            pre_cap *= 2;
            char **np = (char **) realloc(pre, pre_cap * sizeof(char *));
            if (!np) { free(pre); kai_eprintf("kai: native modular merge OOM\n"); return 1; }
            pre = np;
        }
        pre[pre_n++] = strdup(nm);
    }

    LLVMContextRef ctx = LLVMGetModuleContext(mod);
    LLVMModuleRef src = NULL;
    char *err = NULL;
    if (kai_spare_state == 2 && kai_spare_ctx == ctx && kai_spare_mod) {
        src = kai_spare_mod;
        kai_spare_mod = NULL;
    } else {
        LLVMMemoryBufferRef buf = NULL;
        if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &err)) {
            kai_eprintf("kai: native runtime inline bitcode unreadable (%s): %s\n",
                    bc_path, err ? err : "?");
            if (err) LLVMDisposeMessage(err);
            for (size_t i = 0; i < pre_n; i++) free(pre[i]);
            free(pre);
            return 1;
        }
        err = NULL;
        if (LLVMParseIRInContext(ctx, buf, &src, &err)) {
            kai_eprintf("kai: native runtime inline bitcode parse failed (%s): %s\n",
                    bc_path, err ? err : "?");
            if (err) LLVMDisposeMessage(err);
            for (size_t i = 0; i < pre_n; i++) free(pre[i]);
            free(pre);
            return 1;
        }
        if (kai_llvm_trim_runtime_bc(src)) {
            kai_eprintf("kai: native runtime inline bitcode trim left a block in use (%s)\n", bc_path);
            for (size_t i = 0; i < pre_n; i++) free(pre[i]);
            free(pre);
            return 1;
        }
    }
    {
        const char *dst_triple = LLVMGetTarget(mod);
        if (dst_triple) LLVMSetTarget(src, dst_triple);
        LLVMSetModuleDataLayout(src, LLVMGetModuleDataLayout(mod));
    }
    if (LLVMLinkModules2(mod, src)) {
        kai_eprintf("kai: native runtime inline bitcode link failed (%s)\n", bc_path);
        for (size_t i = 0; i < pre_n; i++) free(pre[i]);
        free(pre);
        return 1;
    }

    /* A merged runtime body the owner exports (external in the bitcode) becomes
     * `available_externally`: O2 may still inline it, and a copy it does not
     * consume is dropped before codegen, leaving a call the owner resolves.
     * The bitcode's static helpers go internal. User fns (in the pre set) and
     * the runtime's external state globals are untouched. */
    for (LLVMValueRef f = LLVMGetFirstFunction(mod); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;
        const char *nm = LLVMGetValueName(f);
        if (!nm) continue;
        int was_user = 0;
        for (size_t i = 0; i < pre_n; i++) {
            if (strcmp(pre[i], nm) == 0) { was_user = 1; break; }
        }
        if (was_user) continue;
        LLVMLinkage l = LLVMGetLinkage(f);
        int exported = l == LLVMExternalLinkage || l == LLVMAvailableExternallyLinkage;
        LLVMSetLinkage(f, exported ? LLVMAvailableExternallyLinkage : LLVMInternalLinkage);
    }

    for (size_t i = 0; i < pre_n; i++) free(pre[i]);
    free(pre);
    return 0;
}

#if KAI_LLVM_STATE_HERE
static void *kai_spare_parse(void *arg) {
    char *bc_path = (char *) arg;
    LLVMMemoryBufferRef buf = NULL;
    char *err = NULL;
    if (LLVMCreateMemoryBufferWithContentsOfFile(bc_path, &buf, &err)) {
        if (err) LLVMDisposeMessage(err);
    } else if (LLVMParseIRInContext(kai_spare_ctx, buf, &kai_spare_mod, &err)) {
        if (err) LLVMDisposeMessage(err);
        kai_spare_mod = NULL;
    } else if (kai_llvm_trim_runtime_bc(kai_spare_mod)) {
        kai_spare_mod = NULL;
    }
    free(bc_path);
    return NULL;
}

static void kai_spare_join_at_exit(void) {
    if (kai_spare_state == 1) { pthread_join(kai_spare_th, NULL); kai_spare_state = 2; }
}

/* A native emit names the runtime bitcode in its environment; parsing it now
 * takes the work off the path between the last pass and the object. */
__attribute__((constructor)) static void kai_spare_boot(void) {
    const char *bc = getenv("KAI_NATIVE_RUNTIME_INLINE_BC");
    if (!bc || !bc[0]) return;
    kai_spare_ctx = LLVMContextCreate();
    if (pthread_create(&kai_spare_th, NULL, kai_spare_parse, strdup(bc)) != 0) {
        LLVMContextDispose(kai_spare_ctx);
        kai_spare_ctx = NULL;
        return;
    }
    kai_spare_state = 1;
    atexit(kai_spare_join_at_exit);
}
#endif

/* Copy one function attribute (by name) from `src` onto `dst` if `dst` lacks it. */
static void kai_llvm_copy_fn_str_attr(LLVMValueRef dst, LLVMValueRef src,
                                      const char *name, unsigned nlen) {
    if (LLVMGetStringAttributeAtIndex(dst, LLVMAttributeFunctionIndex, name, nlen)) return;
    LLVMAttributeRef a = LLVMGetStringAttributeAtIndex(src, LLVMAttributeFunctionIndex, name, nlen);
    if (a) LLVMAddAttributeAtIndex(dst, LLVMAttributeFunctionIndex, a);
}

/* Give every emitted program function the SAME `target-cpu`/`target-features`
 * the linked runtime bitcode carries, so the inliner sees caller ⊇ callee and
 * folds the `kaix_*` ops into the hot path — without this the featureless
 * program function is not a superset of the featured runtime callee and
 * `areInlineCompatible` refuses every runtime-op inline. The attributes are
 * copied from a runtime function already in the module (clang baked host
 * features into the bc), which guarantees an exact match regardless of what
 * `LLVMGetHostCPUFeatures` reports on this libLLVM build. MUST run after the bc
 * is linked (so a donor is present) and before the opt pipeline. No-op when no
 * runtime function carries the attributes (the legacy cc-links path). */
static void kai_llvm_stamp_host_features(LLVMModuleRef m) {
    LLVMValueRef donor = NULL;
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;
        if (LLVMGetStringAttributeAtIndex(f, LLVMAttributeFunctionIndex, "target-features", 15)) {
            donor = f; break;
        }
    }
    if (!donor) return;
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f)) continue;
        kai_llvm_copy_fn_str_attr(f, donor, "target-cpu", 10);
        kai_llvm_copy_fn_str_attr(f, donor, "target-features", 15);
    }
}

/* Registers the native target and, for a wasm build, the WebAssembly one.
 * A kai built without it says so instead of failing in the lookup. */
static int kai_llvm_init_targets(void) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    if (!kai_llvm_wasm_target()) return 0;
#ifdef KAI_LLVM_WEBASSEMBLY
    LLVMInitializeWebAssemblyTargetInfo();
    LLVMInitializeWebAssemblyTarget();
    LLVMInitializeWebAssemblyTargetMC();
    LLVMInitializeWebAssemblyAsmPrinter();
    LLVMInitializeWebAssemblyAsmParser();
    static const char *eh[] = { "kai", "-wasm-enable-sjlj", "-wasm-use-legacy-eh=false" };
    LLVMParseCommandLineOptions(3, eh, NULL);
    return 0;
#else
    kai_eprintf("kai: this kai was built without the WebAssembly LLVM target\n");
    return 1;
#endif
}

#ifdef KAI_LLVM_WEBASSEMBLY
extern void kai_llvm_select_wasm_exception_model(LLVMTargetMachineRef tm);
#endif

static LLVMTargetMachineRef kai_llvm_create_target_machine(LLVMTargetRef target, const char *triple) {
    if (kai_llvm_wasm_target()) {
        LLVMTargetMachineRef tm = LLVMCreateTargetMachine(target, triple, "generic",
            "+exception-handling,+mutable-globals,+sign-ext,+bulk-memory,+nontrapping-fptoint,+reference-types,+multivalue",
            kai_llvm_cgen_level(), LLVMRelocStatic, LLVMCodeModelDefault);
#ifdef KAI_LLVM_WEBASSEMBLY
        if (tm) kai_llvm_select_wasm_exception_model(tm);
#endif
        return tm;
    }
    char *cpu = LLVMGetHostCPUName();
    char *features = LLVMGetHostCPUFeatures();
    LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
        target, triple, cpu ? cpu : "", features ? features : "",
        kai_llvm_cgen_level(), LLVMRelocPIC, LLVMCodeModelDefault);
    if (cpu) LLVMDisposeMessage(cpu);
    if (features) LLVMDisposeMessage(features);
    return tm;
}

/* The wasm lowerings find `setjmp` by name and accept only the wasm C++
 * personality on a function with cleanup pads. */
static void kai_llvm_wasm_prepare(LLVMModuleRef m) {
    LLVMValueRef sj = LLVMGetNamedFunction(m, "_setjmp");
    if (sj) {
        LLVMValueRef plain = LLVMGetNamedFunction(m, "setjmp");
        if (plain) { LLVMReplaceAllUsesWith(sj, plain); LLVMDeleteFunction(sj); }
        else LLVMSetValueName2(sj, "setjmp", 6);
    }
    LLVMContextRef cx = LLVMGetModuleContext(m);
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        size_t n = 0;
        const char *nm = LLVMGetValueName2(f, &n);
        if (!LLVMIsDeclaration(f) || n <= 7 || strncmp(nm, "kai_js:", 7) != 0) continue;
        static const char mod[] = "kai_js";
        LLVMAddAttributeAtIndex(f, LLVMAttributeFunctionIndex,
            LLVMCreateStringAttribute(cx, "wasm-import-module", 18, mod, sizeof mod - 1));
        LLVMAddAttributeAtIndex(f, LLVMAttributeFunctionIndex,
            LLVMCreateStringAttribute(cx, "wasm-import-name", 16, nm + 7, (unsigned) (n - 7)));
    }
    LLVMValueRef pers = NULL;
    for (LLVMValueRef f = LLVMGetFirstFunction(m); f; f = LLVMGetNextFunction(f)) {
        if (LLVMIsDeclaration(f) || !LLVMHasPersonalityFn(f)) continue;
        if (!pers) {
            LLVMTypeRef pty = LLVMFunctionType(LLVMInt32TypeInContext(LLVMGetModuleContext(m)), NULL, 0, 1);
            pers = LLVMGetNamedFunction(m, "__gxx_wasm_personality_v0");
            if (!pers) pers = LLVMAddFunction(m, "__gxx_wasm_personality_v0", pty);
        }
        LLVMSetPersonalityFn(f, pers);
    }
}

/* --- object emission --- */
/* Verify the module, then emit it as a native object file at `path`
 * using the host target machine. Returns 0 on success, non-zero on a
 * verify or codegen failure (the driver surfaces the error and aborts).
 * One process: no `.ll` text, no `clang` subprocess. */
static int64_t kai_llvm_emit_object_impl(void *m, const char *out, int link_runtime) {
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
        kai_eprintf("kai: native module verify failed: %s\n", err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        return 1;
    }
    if (err) { LLVMDisposeMessage(err); err = NULL; }

    if (kai_llvm_init_targets()) return 1;

    char *triple = kai_llvm_target_triple();
    LLVMTargetRef target = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &err)) {
        kai_eprintf("kai: native target lookup failed: %s\n", err ? err : "?");
        if (err) LLVMDisposeMessage(err);
        LLVMDisposeMessage(triple);
        return 1;
    }
    LLVMTargetMachineRef tm = kai_llvm_create_target_machine(target, triple);
    LLVMSetTarget((LLVMModuleRef) m, triple);
    LLVMSetModuleDataLayout((LLVMModuleRef) m, LLVMCreateTargetDataLayout(tm));

    /* P2: link the runtime as bitcode BEFORE opt, so default<O2> sees the
     * `kaix_*` bodies and inlines them (no-op when KAI_NATIVE_RUNTIME_BC is
     * unset — the legacy cc-links-runtime_llvm.c path). Runs after the
     * triple/datalayout are set (they must match the bitcode's) and before
     * the pipeline. A real link failure aborts before EmitToFile.
     *
     * `link_runtime`: 1 = whole-program (merge the full runtime bc, internalise
     * all-but-main); 2 = modular partition (merge the SEPARATE-COMPILATION
     * runtime bc whose state globals are external-owned, internalise only the
     * merged runtime FUNCTIONS so O2 inlines the `kaix_*` ops without
     * duplicating runtime state or stripping cross-TU user linkage); 0 = no
     * merge (the runtime is one owner TU linked by cc). */
    int merge_rc = 0;
    if (link_runtime == 1)      merge_rc = kai_llvm_link_runtime_bc(m);
    else if (link_runtime == 2) merge_rc = kai_llvm_link_runtime_bc_modular(m);
    if (merge_rc) {
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeMessage(triple);
        return 1;
    }

    /* Propagate the runtime bc's host target-cpu/features onto the emitted
     * program functions so the opt pipeline can inline the runtime ops. Runs
     * on the linked module (donor present) and before the passes. */
    kai_llvm_stamp_host_features((LLVMModuleRef) m);
    if (kai_llvm_wasm_target()) kai_llvm_wasm_prepare((LLVMModuleRef) m);

    /* L4 (issue #498): optimise the module in-process before codegen.
     * Default `default<O2>` for parity with the clang `-O2` the C/LLVM-
     * text paths get; `KAI_NATIVE_OPT=0` (bin/kai --debug) drops to O0.
     * A pass error aborts before EmitToFile (no half-optimised object). */
    if (kai_llvm_run_passes((LLVMModuleRef) m, tm, link_runtime != 0)) {
        LLVMDisposeTargetMachine(tm);
        LLVMDisposeMessage(triple);
        return 1;
    }

    /* ELF garbage-collects at section granularity, and the C API has no
     * FunctionSections toggle, so stamp every defined function into its own
     * .text.<name> section; the executable link then passes --gc-sections.
     * Mach-O needs nothing here: LLVM emits subsections-via-symbols and the
     * link dead-strips per atom. Unconditional so a content-addressed object
     * never depends on the linker flag being on. */
#if !defined(__APPLE__) && !defined(_WIN32)
    for (LLVMValueRef fsec = LLVMGetFirstFunction((LLVMModuleRef) m); fsec;
         fsec = LLVMGetNextFunction(fsec)) {
        if (LLVMIsDeclaration(fsec)) continue;
        const char *cursec = LLVMGetSection(fsec);
        if (cursec && cursec[0]) continue;
        size_t fnlen = 0;
        const char *fname = LLVMGetValueName2(fsec, &fnlen);
        if (!fname || fnlen == 0) continue;
        char *secname = (char *) malloc(fnlen + 7);
        if (!secname) continue;
        memcpy(secname, ".text.", 6);
        memcpy(secname + 6, fname, fnlen);
        secname[fnlen + 6] = '\0';
        LLVMSetSection(fsec, secname);
        free(secname);
    }
#endif

    /* Atomic publish: emit to a pid-suffixed sibling, then rename() onto the
     * final path. A content-addressed object (the shared core cache) can be
     * read by a concurrent build the moment it exists, so it must never be
     * observable half-written; rename within one directory is atomic. */
    {
        char tmp_out[4096];
        snprintf(tmp_out, sizeof tmp_out, "%s.tmp%ld", out, (long) getpid());
        if (LLVMTargetMachineEmitToFile(tm, (LLVMModuleRef) m, tmp_out, LLVMObjectFile, &err)) {
            kai_eprintf("kai: native object emit failed: %s: %s\n", tmp_out, err ? err : "?");
            if (err) LLVMDisposeMessage(err);
            unlink(tmp_out);
            rc = 1;
        } else if (rename(tmp_out, out) != 0) {
            kai_eprintf("kai: native object publish failed: %s -> %s\n", tmp_out, out);
            unlink(tmp_out);
            rc = 1;
        }
    }

    LLVMDisposeTargetMachine(tm);
    LLVMDisposeMessage(triple);
    return rc;
}

/* Partition emit pool. With KAI_NATIVE_JOBS > 1 a partition's verify, runtime
 * merge, O2 and emit run on a worker while the walk builds the next partition.
 * A partition owns its LLVMContext, so a worker shares no LLVM state with the
 * walk or another worker, and it never touches the kaikai heap. The `exit`
 * prim drains the queue, so every queued object is published before the
 * process exits; a failed one exits 1. */
typedef struct KaiEmitJob { LLVMModuleRef m; char *out; struct KaiEmitJob *next; } KaiEmitJob;

#define KAI_EMIT_MAX_WORKERS 64
#if defined(KAI_SEPARATE_COMPILATION)
extern pthread_mutex_t kai_emit_mu;
extern pthread_cond_t kai_emit_work_cv;
extern pthread_cond_t kai_emit_idle_cv;
extern KaiEmitJob *kai_emit_head;
extern KaiEmitJob *kai_emit_tail;
extern pthread_t kai_emit_threads[KAI_EMIT_MAX_WORKERS];
extern int kai_emit_nworkers;
extern int kai_emit_started;
extern int kai_emit_busy;
extern int kai_emit_failed;
extern int kai_emit_stop;
extern char **kai_emit_outs;
extern size_t kai_emit_nouts;
extern size_t kai_emit_capouts;
#endif
#if KAI_LLVM_STATE_HERE
KAI_LLVM_STATE pthread_mutex_t kai_emit_mu = PTHREAD_MUTEX_INITIALIZER;
KAI_LLVM_STATE pthread_cond_t kai_emit_work_cv = PTHREAD_COND_INITIALIZER;
KAI_LLVM_STATE pthread_cond_t kai_emit_idle_cv = PTHREAD_COND_INITIALIZER;
KAI_LLVM_STATE KaiEmitJob *kai_emit_head = NULL;
KAI_LLVM_STATE KaiEmitJob *kai_emit_tail = NULL;
KAI_LLVM_STATE pthread_t kai_emit_threads[KAI_EMIT_MAX_WORKERS];
KAI_LLVM_STATE int kai_emit_nworkers = -1;
KAI_LLVM_STATE int kai_emit_started = 0;
KAI_LLVM_STATE int kai_emit_busy = 0;
KAI_LLVM_STATE int kai_emit_failed = 0;
KAI_LLVM_STATE int kai_emit_stop = 0;
KAI_LLVM_STATE char **kai_emit_outs = NULL;
KAI_LLVM_STATE size_t kai_emit_nouts = 0;
KAI_LLVM_STATE size_t kai_emit_capouts = 0;
#endif

static int kai_emit_pool_on(void) {
    if (kai_emit_nworkers < 0) {
        const char *e = getenv("KAI_NATIVE_JOBS");
        long n = (e && e[0]) ? strtol(e, NULL, 10) : 1;
        kai_emit_nworkers = n > KAI_EMIT_MAX_WORKERS ? KAI_EMIT_MAX_WORKERS : (int) n;
    }
    return kai_emit_nworkers > 1;
}

static void *kai_emit_worker(void *arg) {
    (void) arg;
    for (;;) {
        pthread_mutex_lock(&kai_emit_mu);
        while (!kai_emit_head && !kai_emit_stop) pthread_cond_wait(&kai_emit_work_cv, &kai_emit_mu);
        KaiEmitJob *job = kai_emit_head;
        if (!job) { pthread_mutex_unlock(&kai_emit_mu); return NULL; }
        kai_emit_head = job->next;
        if (!kai_emit_head) kai_emit_tail = NULL;
        pthread_mutex_unlock(&kai_emit_mu);
        int64_t rc = kai_llvm_emit_object_impl(job->m, job->out, 2);
        free(job->out);
        free(job);
        pthread_mutex_lock(&kai_emit_mu);
        if (rc) kai_emit_failed++;
        if (--kai_emit_busy == 0) pthread_cond_broadcast(&kai_emit_idle_cv);
        pthread_mutex_unlock(&kai_emit_mu);
    }
}

static void kai_emit_pool_drain(int code) {
    if (!kai_emit_started) return;
    pthread_mutex_lock(&kai_emit_mu);
    while (code != 0 && kai_emit_head) {
        KaiEmitJob *job = kai_emit_head;
        kai_emit_head = job->next;
        free(job->out);
        free(job);
        kai_emit_busy--;
    }
    kai_emit_tail = kai_emit_head;
    kai_emit_stop = 1;
    pthread_cond_broadcast(&kai_emit_work_cv);
    while (kai_emit_busy > 0) pthread_cond_wait(&kai_emit_idle_cv, &kai_emit_mu);
    int failed = kai_emit_failed;
    kai_emit_started = 0;
    pthread_mutex_unlock(&kai_emit_mu);
    for (int i = 0; i < kai_emit_nworkers; i++) pthread_join(kai_emit_threads[i], NULL);
    if (failed && code == 0) { fflush(NULL); _exit(1); }
}

/* Workers block SIGCHLD so the reactor's wake signal stays on the scheduler. */
static void kai_emit_pool_start(void) {
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeAsmPrinter();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t) 64 << 20);
    sigset_t block_set, prev_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block_set, &prev_set);
    for (int i = 0; i < kai_emit_nworkers; i++) {
        kai_rc_go_mt();
        if (pthread_create(&kai_emit_threads[i], &attr, kai_emit_worker, NULL) != 0) {
            kai_eprintf("kai: native emit pthread_create failed: %s\n", strerror(errno));
            _exit(1);
        }
    }
    pthread_sigmask(SIG_SETMASK, &prev_set, NULL);
    pthread_attr_destroy(&attr);
    kai_emit_started = 1;
}

/* An object already queued is not queued again: outputs are content-addressed,
 * and two writers of one path would race on its temp name. */
static int kai_emit_seen(const char *out) {
    for (size_t i = 0; i < kai_emit_nouts; i++) if (strcmp(kai_emit_outs[i], out) == 0) return 1;
    if (kai_emit_nouts == kai_emit_capouts) {
        kai_emit_capouts = kai_emit_capouts ? kai_emit_capouts * 2 : 64;
        kai_emit_outs = (char **) realloc(kai_emit_outs, kai_emit_capouts * sizeof(char *));
    }
    kai_emit_outs[kai_emit_nouts++] = strdup(out);
    return 0;
}

static int64_t kai_emit_pool_submit(LLVMModuleRef m, const char *out) {
    pthread_mutex_lock(&kai_emit_mu);
    if (!kai_emit_started) kai_emit_pool_start();
    if (kai_emit_seen(out)) { pthread_mutex_unlock(&kai_emit_mu); return 0; }
    KaiEmitJob *job = (KaiEmitJob *) malloc(sizeof(KaiEmitJob));
    job->m = m;
    job->out = strdup(out);
    job->next = NULL;
    if (kai_emit_tail) kai_emit_tail->next = job; else kai_emit_head = job;
    kai_emit_tail = job;
    kai_emit_busy++;
    pthread_cond_signal(&kai_emit_work_cv);
    pthread_mutex_unlock(&kai_emit_mu);
    /* Test hook: an exit taken while partitions are in flight. */
    const char *after = getenv("KAI_NATIVE_EMIT_EXIT_AFTER");
    if (after && (size_t) strtol(after, NULL, 10) == kai_emit_nouts) kai_exit(3);
    return 0;
}

/* Whole-program object: merge the runtime bitcode + internalise so O2 inlines
 * the `kaix_*` bodies. The default single-TU native path. */
static int64_t kai_llvm_emit_object(void *m, KaiValue *path) {
    int64_t rc = kai_llvm_emit_object_impl(m, path->as.s.bytes, 1);
    kai_decref(path);
    return rc;
}

/* One partition of a modular build. With KAI_NATIVE_RUNTIME_INLINE_BC set,
 * merges the separate-compilation runtime bc so O2 inlines the `kaix_*` hot
 * ops into this partition (runtime state stays external, owned by the runtime
 * TU — no per-partition duplication). Unset → no merge, the runtime is one
 * owner TU linked by cc (the legacy path). Cross-TU user symbols keep external
 * linkage either way. */
static int64_t kai_llvm_emit_object_raw(void *m, KaiValue *path) {
    int64_t rc = kai_emit_pool_on() ? kai_emit_pool_submit((LLVMModuleRef) m, path->as.s.bytes)
                                    : kai_llvm_emit_object_impl(m, path->as.s.bytes, 2);
    kai_decref(path);
    return rc;
}
