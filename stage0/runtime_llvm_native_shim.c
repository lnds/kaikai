/* Native self-host shim: linkable forwarders for the in-process libLLVM
 * C-API prims (issue #1021 LINK+RUN).
 *
 * The native backend calls the builder / context / DI prims as
 * `kaix_core_llvm_*`, `kaix_core_native_ctx_*`,
 * `kaix_core_native_di_*` (emit_native_ops `ncall_sym`). Their bodies
 * live in `runtime.h` as `static kai_<name>` — a `static` cannot be named
 * across translation units, so a SEPARATELY-compiled native object (the
 * compiler built with `--backend=native`) cannot bind them. These
 * non-static thunks are that linkable name, mirroring the
 * `kaix_core_print` etc. forwarders in `runtime_llvm.c`.
 *
 * WHY A SEPARATE TU (not folded into runtime_llvm.c): the native compiler
 * object is bitcode-self-contained (runtime_llvm.bc is linked in-process),
 * so it already defines `main` + every other `kaix_*`. It lacks ONLY these
 * libLLVM prims, because `runtime_llvm.bc` is generated WITHOUT `-DKAI_LLVM`
 * (gen-runtime-bc.sh). Compiling all of `runtime_llvm.c` here would
 * duplicate `main` + the whole runtime; this TU carries the missing
 * libLLVM-prim symbols alone.
 *
 * Only meaningful under `-DKAI_LLVM` (the whole `runtime.h` LLVM block is
 * `#ifdef KAI_LLVM`); without it this file is empty. Kept in lockstep with
 * `native_prims.kai`'s `rcore_table` and `runtime.h`'s `kai_<name>`.
 *
 * Angle-bracket include, like runtime_llvm.c: `<runtime.h>` obeys the `-I`
 * search order (stage2 ahead of stage0), binding to the Koka runtime.
 */
#include <runtime.h>

#ifdef KAI_LLVM
KaiValue * kaix_core_llvm_add_byval_call(void *m, void *call, int64_t param_ix, void *sty) { return kai_llvm_add_byval_call(m, call, param_ix, sty); }
KaiValue * kaix_core_llvm_add_byval_decl(void *m, void *fn, int64_t param_ix, void *sty) { return kai_llvm_add_byval_decl(m, fn, param_ix, sty); }
KaiValue * kaix_core_llvm_add_case(void *sw, void *onval, void *bb) { return kai_llvm_add_case(sw, onval, bb); }
void * kaix_core_llvm_add_function(void *m, KaiValue *name, void *fnty) { return kai_llvm_add_function(m, name, fnty); }
void * kaix_core_llvm_add_global_zeroed(void *m, void *ty, KaiValue *name) { return kai_llvm_add_global_zeroed(m, ty, name); }
void * kaix_core_llvm_add_global_extern_def(void *m, void *ty, KaiValue *name) { return kai_llvm_add_global_extern_def(m, ty, name); }
void * kaix_core_llvm_add_global_extern_decl(void *m, void *ty, KaiValue *name) { return kai_llvm_add_global_extern_decl(m, ty, name); }
KaiValue * kaix_core_llvm_add_sret_call(void *m, void *call, void *sty) { return kai_llvm_add_sret_call(m, call, sty); }
KaiValue * kaix_core_llvm_add_sret_decl(void *m, void *fn, void *sty) { return kai_llvm_add_sret_decl(m, fn, sty); }
KaiValue * kaix_core_llvm_add_nounwind(void *fn) { return kai_llvm_add_nounwind(fn); }
KaiValue * kaix_core_llvm_add_memory_none(void *fn) { return kai_llvm_add_memory_none(fn); }
KaiValue * kaix_core_llvm_set_internal_linkage(void *fn) { return kai_llvm_set_internal_linkage(fn); }
void * kaix_core_llvm_append_block(void *m, void *fn, KaiValue *name) { return kai_llvm_append_block(m, fn, name); }
void * kaix_core_llvm_array_type(void *elem, int64_t n) { return kai_llvm_array_type(elem, n); }
KaiValue * kaix_core_llvm_buf_free(void *buf) { return kai_llvm_buf_free(buf); }
void * kaix_core_llvm_buf_get(void *buf, int64_t i) { return kai_llvm_buf_get(buf, i); }
int64_t kaix_core_llvm_buf_len(void *buf) { return kai_llvm_buf_len(buf); }
void * kaix_core_llvm_buf_new(void) { return kai_llvm_buf_new(); }
KaiValue * kaix_core_llvm_buf_push(void *buf, void *h) { return kai_llvm_buf_push(buf, h); }
void * kaix_core_llvm_build_alloca(void *b, void *ty, KaiValue *name) { return kai_llvm_build_alloca(b, ty, name); }
void * kaix_core_llvm_build_alloca_entry(void *cv, void *ty, KaiValue *name) { return kai_llvm_build_alloca_entry(cv, ty, name); }
void * kaix_core_llvm_build_array_alloca(void *b, void *elemty, void *count, KaiValue *name) { return kai_llvm_build_array_alloca(b, elemty, count, name); }
void * kaix_core_llvm_build_array_gep(void *b, void *arrty, void *arr, void *idx) { return kai_llvm_build_array_gep(b, arrty, arr, idx); }
KaiValue * kaix_core_llvm_build_br(void *b, void *bb) { return kai_llvm_build_br(b, bb); }
void * kaix_core_llvm_build_call_n(void *b, void *fn, void *fnty, void *buf) { return kai_llvm_build_call_n(b, fn, fnty, buf); }
KaiValue * kaix_core_llvm_build_cond_br(void *b, void *cond, void *then_bb, void *else_bb) { return kai_llvm_build_cond_br(b, cond, then_bb, else_bb); }
void * kaix_core_llvm_build_fbinop(void *b, int64_t op, void *a, void *c) { return kai_llvm_build_fbinop(b, op, a, c); }
void * kaix_core_llvm_build_fcmp(void *b, int64_t pred, void *a, void *c) { return kai_llvm_build_fcmp(b, pred, a, c); }
void * kaix_core_llvm_build_fneg(void *b, void *a) { return kai_llvm_build_fneg(b, a); }
void * kaix_core_llvm_build_fpcast(void *b, void *v, void *ty) { return kai_llvm_build_fpcast(b, v, ty); }
void * kaix_core_llvm_build_global_string(void *b, KaiValue *s) { return kai_llvm_build_global_string(b, s); }
void * kaix_core_llvm_build_ibinop(void *b, int64_t op, void *a, void *c) { return kai_llvm_build_ibinop(b, op, a, c); }
void * kaix_core_llvm_build_icmp(void *b, int64_t pred, void *a, void *c) { return kai_llvm_build_icmp(b, pred, a, c); }
void * kaix_core_llvm_build_ucmp(void *b, int64_t pred, void *a, void *c) { return kai_llvm_build_ucmp(b, pred, a, c); }
void * kaix_core_llvm_build_icmp_ne_zero(void *b, void *v, void *i32ty) { return kai_llvm_build_icmp_ne_zero(b, v, i32ty); }
void * kaix_core_llvm_build_lnot(void *b, void *a) { return kai_llvm_build_lnot(b, a); }
void * kaix_core_llvm_build_load(void *b, void *ty, void *ptr) { return kai_llvm_build_load(b, ty, ptr); }
void * kaix_core_llvm_build_load_arg(void *b, void *args, void *ptrt, int64_t i) { return kai_llvm_build_load_arg(b, args, ptrt, i); }
void * kaix_core_llvm_build_logical(void *b, int64_t op, void *a, void *c) { return kai_llvm_build_logical(b, op, a, c); }
KaiValue * kaix_core_llvm_build_ret(void *b, void *v) { return kai_llvm_build_ret(b, v); }
KaiValue * kaix_core_llvm_build_ret_void(void *b) { return kai_llvm_build_ret_void(b); }
void * kaix_core_llvm_build_sext(void *b, void *v, void *ty) { return kai_llvm_build_sext(b, v, ty); }
void * kaix_core_llvm_build_ptrtoint(void *b, void *v, void *ty) { return kai_llvm_build_ptrtoint(b, v, ty); }
void * kaix_core_llvm_build_inttoptr(void *b, void *v, void *ty) { return kai_llvm_build_inttoptr(b, v, ty); }
KaiValue * kaix_core_llvm_build_store(void *b, void *val, void *ptr) { return kai_llvm_build_store(b, val, ptr); }
void * kaix_core_llvm_build_string_span(void *b, KaiValue *s) { return kai_llvm_build_string_span(b, s); }
void * kaix_core_llvm_build_struct_gep(void *b, void *sty, void *ptr, int64_t i) { return kai_llvm_build_struct_gep(b, sty, ptr, i); }
void * kaix_core_llvm_build_switch(void *b, void *val, void *default_bb, int64_t ncases) { return kai_llvm_build_switch(b, val, default_bb, ncases); }
void * kaix_core_llvm_build_trunc(void *b, void *v, void *ty) { return kai_llvm_build_trunc(b, v, ty); }
KaiValue * kaix_core_llvm_build_unreachable(void *b) { return kai_llvm_build_unreachable(b); }
void * kaix_core_llvm_build_zext(void *b, void *v, void *ty) { return kai_llvm_build_zext(b, v, ty); }
void * kaix_core_llvm_build_zext_i1_i32(void *b, void *v, void *i32ty) { return kai_llvm_build_zext_i1_i32(b, v, i32ty); }
void * kaix_core_llvm_const_i32(void *i32ty, int64_t v) { return kai_llvm_const_i32(i32ty, v); }
void * kaix_core_llvm_const_int(void *i64ty, int64_t v) { return kai_llvm_const_int(i64ty, v); }
void * kaix_core_llvm_const_null(void *ptr_t) { return kai_llvm_const_null(ptr_t); }
void * kaix_core_llvm_const_real(void *f64ty, double d) { return kai_llvm_const_real(f64ty, d); }
int64_t kaix_core_llvm_emit_object(void *m, KaiValue *path) { return kai_llvm_emit_object(m, path); }
int64_t kaix_core_llvm_emit_object_raw(void *m, KaiValue *path) { return kai_llvm_emit_object_raw(m, path); }
void * kaix_core_llvm_float_type(void *m) { return kai_llvm_float_type(m); }
void * kaix_core_llvm_fn_type_0(void *ret) { return kai_llvm_fn_type_0(ret); }
void * kaix_core_llvm_fn_type_boxed(void *ptr_t, int64_t n) { return kai_llvm_fn_type_boxed(ptr_t, n); }
void * kaix_core_llvm_fn_type_n(void *ret, void *buf) { return kai_llvm_fn_type_n(ret, buf); }
void * kaix_core_llvm_get_named_global(void *m, KaiValue *name) { return kai_llvm_get_named_global(m, name); }
void * kaix_core_llvm_get_or_declare_fn(void *m, KaiValue *name, void *fnty) { return kai_llvm_get_or_declare_fn(m, name, fnty); }
void * kaix_core_llvm_get_param(void *fn, int64_t i) { return kai_llvm_get_param(fn, i); }
int32_t kaix_core_llvm_handle_is_null(void *h) { return kai_llvm_handle_is_null(h); }
void * kaix_core_llvm_int_type(void *m, int64_t bits) { return kai_llvm_int_type(m, bits); }
void * kaix_core_llvm_module_new(KaiValue *name) { return kai_llvm_module_new(name); }
KaiValue * kaix_core_llvm_position_at_end(void *b, void *bb) { return kai_llvm_position_at_end(b, bb); }
void * kaix_core_llvm_struct_type(void *m, void *buf) { return kai_llvm_struct_type(m, buf); }
KaiValue * kaix_core_native_ctx_add_block(void *cv, KaiValue *label, void *bb) { return kai_native_ctx_add_block(cv, label, bb); }
KaiValue * kaix_core_native_ctx_add_frame_slot(void *cv, KaiValue *symv, KaiValue *effv) { return kai_native_ctx_add_frame_slot(cv, symv, effv); }
KaiValue * kaix_core_native_ctx_add_reg(void *cv, KaiValue *name, void *alloca, int64_t slot) { return kai_native_ctx_add_reg(cv, name, alloca, slot); }
void * kaix_core_native_ctx_b(void *c) { return kai_native_ctx_b(c); }
KaiValue * kaix_core_native_ctx_begin_fn(void *cv, void *fnval) { return kai_native_ctx_begin_fn(cv, fnval); }
KaiValue * kaix_core_native_ctx_end_fn(void *cv) { return kai_native_ctx_end_fn(cv); }
void * kaix_core_native_ctx_f64t(void *c) { return kai_native_ctx_f64t(c); }
KaiValue * kaix_core_native_ctx_fail(void *c) { return kai_native_ctx_fail(c); }
void * kaix_core_native_ctx_find_block(void *cv, KaiValue *label) { return kai_native_ctx_find_block(cv, label); }
void * kaix_core_native_ctx_find_reg(void *cv, KaiValue *name) { return kai_native_ctx_find_reg(cv, name); }
void * kaix_core_native_ctx_first_block(void *cv) { return kai_native_ctx_first_block(cv); }
void * kaix_core_native_ctx_fnval(void *c) { return kai_native_ctx_fnval(c); }
int64_t kaix_core_native_ctx_frame_slot_count(void *cv, KaiValue *symv) { return kai_native_ctx_frame_slot_count(cv, symv); }
KaiValue * kaix_core_native_ctx_frame_slot_eff(void *cv, KaiValue *symv, int64_t j) { return kai_native_ctx_frame_slot_eff(cv, symv, j); }
int64_t kaix_core_native_ctx_frame_slot_index(void *cv, KaiValue *symv, KaiValue *effv) { return kai_native_ctx_frame_slot_index(cv, symv, effv); }
void * kaix_core_native_ctx_i32t(void *c) { return kai_native_ctx_i32t(c); }
void * kaix_core_native_ctx_i64t(void *c) { return kai_native_ctx_i64t(c); }
void * kaix_core_native_ctx_m(void *c) { return kai_native_ctx_m(c); }
void * kaix_core_native_ctx_new(void *m) { return kai_native_ctx_new(m); }
int64_t kaix_core_native_ctx_ok(void *c) { return kai_native_ctx_ok(c); }
void * kaix_core_native_ctx_ptrt(void *c) { return kai_native_ctx_ptrt(c); }
void * kaix_core_native_ctx_reg_at(void *cv, int64_t i) { return kai_native_ctx_reg_at(cv, i); }
int64_t kaix_core_native_ctx_reg_slot(void *cv, KaiValue *name) { return kai_native_ctx_reg_slot(cv, name); }
int64_t kaix_core_native_ctx_reg_slot_at(void *cv, int64_t i) { return kai_native_ctx_reg_slot_at(cv, i); }
KaiValue * kaix_core_native_ctx_set_fnval(void *c, void *fn) { return kai_native_ctx_set_fnval(c, fn); }
void * kaix_core_native_ctx_voidt(void *c) { return kai_native_ctx_voidt(c); }
KaiValue * kaix_core_native_di_clear_loc(void *cv) { return kai_native_di_clear_loc(cv); }
KaiValue * kaix_core_native_di_debug_marker(void *cv) { return kai_native_di_debug_marker(cv); }
KaiValue * kaix_core_native_di_enable(void *cv, KaiValue *fnamev, KaiValue *dirv) { return kai_native_di_enable(cv, fnamev, dirv); }
int64_t kaix_core_native_di_enabled(void *cv) { return kai_native_di_enabled(cv); }
KaiValue * kaix_core_native_di_finalize(void *cv) { return kai_native_di_finalize(cv); }
KaiValue * kaix_core_native_di_set_loc(void *cv, int64_t line, int64_t col) { return kai_native_di_set_loc(cv, line, col); }
KaiValue * kaix_core_native_di_subprogram(void *cv, void *fnval, KaiValue *namev, int64_t line) { return kai_native_di_subprogram(cv, fnval, namev, line); }
int64_t kaix_core_native_target_abi(void) { return kai_native_target_abi(); }
KaiValue * kaix_core_llvm_backend_tag(void) { return kai_llvm_backend_tag(); }
#endif /* KAI_LLVM */
