/* Fragment of runtime.h; include runtime.h, never this file. */
/* Default / bootstrap build: libLLVM is not linked, so the C-API
 * forwarders are stubs. They exist only so the emitted compiler (which
 * always contains the `emit_native` code path) LINKS; calling
 * `--emit=native` on a kaic2 built without `KAI_LLVM=1` aborts here with
 * a clear message instead of a link error. The default backend never
 * reaches these (it dispatches to the C-text emitter). */
static void *kai_llvm_native_unavailable(void) {
    kai_eprintf(
        "kai: the native (in-process libLLVM) backend is not built into this "
        "compiler.\n     Rebuild stage2 with `make KAI_LLVM=1` to enable "
        "`--emit=native`.\n");
    kai_exit(1);
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
static void *kai_native_ctx_i128t(void *c) { (void) c; return kai_llvm_native_unavailable(); }
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
static void *kai_llvm_build_ucmp(void *b, int64_t p, void *a, void *c) { (void) b; (void) p; (void) a; (void) c; return kai_llvm_native_unavailable(); }
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
static KaiValue *kai_llvm_add_sret_decl(void *m, void *fn, void *s) { (void) m; (void) fn; (void) s; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_add_sret_call(void *m, void *c, void *s) { (void) m; (void) c; (void) s; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_add_nounwind(void *fn) { (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_add_memory_none(void *fn) { (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_llvm_set_musttail(void *call) { (void) call; kai_llvm_native_unavailable(); return 0; }
static KaiValue *kai_llvm_set_internal_linkage(void *fn) { (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_target_abi(void) { kai_llvm_native_unavailable(); return 0; }
/* Silent (no abort): the driver probes this to decide whether the split
 * core-object cache can engage; "" means it cannot on a C-only kaic2. */
static KaiValue *kai_llvm_backend_tag(void) { return kai_str(""); }
static void *kai_llvm_build_trunc(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_sext(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_zext(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_ptrtoint(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_inttoptr(void *b, void *v, void *ty) { (void) b; (void) v; (void) ty; return kai_llvm_native_unavailable(); }
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
static void *kai_llvm_const_i128_str(void *i128ty, KaiValue *s) { (void) i128ty; (void) s; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_0(void *b, void *fn, void *fnty) { (void) b; (void) fn; (void) fnty; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_1(void *b, void *fn, void *fnty, void *a0) { (void) b; (void) fn; (void) fnty; (void) a0; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_build_ret(void *b, void *v) { (void) b; (void) v; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_build_ret_void(void *b) { (void) b; kai_llvm_native_unavailable(); return kai_unit(); }
/* Parte B generic-walk stubs (same contract: link, then abort on use). */
static void *kai_llvm_fn_type_boxed(void *p, int64_t n) { (void) p; (void) n; return kai_llvm_native_unavailable(); }
static void *kai_llvm_fn_type_n(void *r, void *buf) { (void) r; (void) buf; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_call_n(void *b, void *fn, void *t, void *buf) { (void) b; (void) fn; (void) t; (void) buf; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_invoke_n(void *b, void *fn, void *t, void *buf, void *tb, void *ub) { (void) b; (void) fn; (void) t; (void) buf; (void) tb; (void) ub; return kai_llvm_native_unavailable(); }
static void *kai_llvm_build_landingpad_cleanup(void *b) { (void) b; return kai_llvm_native_unavailable(); }
static KaiValue *kai_native_ctx_set_pad(void *c, void *bb) { (void) c; (void) bb; kai_llvm_native_unavailable(); return kai_unit(); }
static void *kai_native_ctx_pad(void *c) { (void) c; return kai_llvm_native_unavailable(); }
static void *kai_llvm_get_insert_block(void *b) { (void) b; return kai_llvm_native_unavailable(); }
static void *kai_native_build_call(void *c, void *fn, void *t, void *buf) { (void) c; (void) fn; (void) t; (void) buf; return kai_llvm_native_unavailable(); }
static KaiValue *kai_llvm_build_resume(void *b, void *lp) { (void) b; (void) lp; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_set_personality(void *fn, void *pers) { (void) fn; (void) pers; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_llvm_add_uwtable(void *fn) { (void) fn; kai_llvm_native_unavailable(); return kai_unit(); }
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
static void *kai_llvm_add_global_extern_def(void *m, void *t, KaiValue *nm) { (void) m; (void) t; (void) nm; return kai_llvm_native_unavailable(); }
static void *kai_llvm_add_global_extern_decl(void *m, void *t, KaiValue *nm) { (void) m; (void) t; (void) nm; return kai_llvm_native_unavailable(); }
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
static int64_t kai_llvm_emit_object_raw(void *m, KaiValue *path) { (void) m; (void) path; kai_llvm_native_unavailable(); return 1; }
/* DWARF DI stubs (#500): unreachable on the C-only path (the native walk
 * that calls them never runs), so they just satisfy the link. */
static KaiValue *kai_native_di_enable(void *c, KaiValue *f, KaiValue *d) { (void) c; (void) f; (void) d; kai_llvm_native_unavailable(); return kai_unit(); }
static int64_t kai_native_di_enabled(void *c) { (void) c; kai_llvm_native_unavailable(); return 0; }
static KaiValue *kai_native_di_subprogram(void *c, void *fn, KaiValue *n, int64_t l) { (void) c; (void) fn; (void) n; (void) l; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_set_loc(void *c, int64_t l, int64_t col) { (void) c; (void) l; (void) col; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_clear_loc(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_finalize(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
static KaiValue *kai_native_di_debug_marker(void *c) { (void) c; kai_llvm_native_unavailable(); return kai_unit(); }
