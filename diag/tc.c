#include <runtime_llvm.c>
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wunused-label"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#ifdef __APPLE__
#define KC_ASM(s) "_" s
#else
#define KC_ASM(s) s
#endif
#if defined(__clang__)
#define KC_MUSTTAIL __attribute__((musttail))
#define KC_TAIL_FN
#elif defined(__GNUC__) && __GNUC__ >= 15
#define KC_MUSTTAIL [[gnu::musttail]]
#define KC_TAIL_FN __attribute__((noinline))
#else
#define KC_MUSTTAIL
#define KC_TAIL_FN
#endif
#define KD(x) kaix_internal_drop(x)
#define KU(x) kaix_internal_dup(x)
KaiValue * kaiu_spawn__nursery(KaiValue * p0);
KaiValue * kaiu_main__req_bytes(void);
KaiValue * kaiu_main__rep_bytes(void);
int64_t kaiu_main__serve_one(void *p_evf, KaiValue * p0);
int64_t kaiu_main__client_one(void *p_evf, int64_t p0);
KaiValue * kai_main(void);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_109_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_108_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_107_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_106_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_105_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_main_103_30_104_26(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam_main_103_30(KaiValue *p_self, KaiValue **p_args, int p_n);
static int64_t _kai_default_ev_Stdout[5] __attribute__((aligned(16)));
static int64_t _kai_default_node_Stdout[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Stderr[5] __attribute__((aligned(16)));
static int64_t _kai_default_node_Stderr[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Stdin[6] __attribute__((aligned(16)));
static int64_t _kai_default_node_Stdin[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Ffi[3] __attribute__((aligned(16)));
static int64_t _kai_default_node_Ffi[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Js[3] __attribute__((aligned(16)));
static int64_t _kai_default_node_Js[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Env[8] __attribute__((aligned(16)));
static int64_t _kai_default_node_Env[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_File[10] __attribute__((aligned(16)));
static int64_t _kai_default_node_File[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Random[4] __attribute__((aligned(16)));
static int64_t _kai_default_node_Random[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_SecureRandom[5] __attribute__((aligned(16)));
static int64_t _kai_default_node_SecureRandom[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Log[7] __attribute__((aligned(16)));
static int64_t _kai_default_node_Log[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Mutable[11] __attribute__((aligned(16)));
static int64_t _kai_default_node_Mutable[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_State[5] __attribute__((aligned(16)));
static int64_t _kai_default_node_State[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Reader[4] __attribute__((aligned(16)));
static int64_t _kai_default_node_Reader[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Writer[4] __attribute__((aligned(16)));
static int64_t _kai_default_node_Writer[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Actor[7] __attribute__((aligned(16)));
static int64_t _kai_default_node_Actor[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Spawn[11] __attribute__((aligned(16)));
static int64_t _kai_default_node_Spawn[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Cancel[4] __attribute__((aligned(16)));
static int64_t _kai_default_node_Cancel[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Link[4] __attribute__((aligned(16)));
static int64_t _kai_default_node_Link[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Monitor[5] __attribute__((aligned(16)));
static int64_t _kai_default_node_Monitor[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Signal[6] __attribute__((aligned(16)));
static int64_t _kai_default_node_Signal[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_Process[15] __attribute__((aligned(16)));
static int64_t _kai_default_node_Process[11] __attribute__((aligned(16)));
static int64_t _kai_default_ev_NetTcp[10] __attribute__((aligned(16)));
static int64_t _kai_default_node_NetTcp[11] __attribute__((aligned(16)));
KaiValue * kaiu_spawn__nursery(KaiValue * p0) {
    KaiValue *r_body_0, *t0, *t1, *t2, *t3, *t4, *r_r_1, *t5, *t6, *t7;
    KaiCont r___kai_cont;
    KaiFiber *kc_unw_fiber = NULL;
    r_body_0 = p0;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_body_0 }), 1LL);
    t2 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t1);
    KD(t2);
    t3 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_body_0 }), 1LL);
    t4 = kaix_apply_borrow(r_body_0, (int32_t)0LL, NULL);
    (void)kaix_unw_pop((KaiFiber *) t3);
    KD(r_body_0);
    r_r_1 = t4;
    t5 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_r_1 }), 1LL);
    t6 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiCont *)) kaix_ev_op_at(h_, 10))(h_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t5);
    KD(t6);
    t7 = r_r_1;
    return t7;
}

KaiValue * kaiu_main__req_bytes(void) {
    KaiValue *t0, *t1, *t2;
L_entry:;
    t0 = kaix_int(80LL);
    t1 = kaix_nil();
    t2 = kaix_cons(t0, t1);
    return t2;
}

KaiValue * kaiu_main__rep_bytes(void) {
    KaiValue *t0, *t1, *t2;
L_entry:;
    t0 = kaix_int(75LL);
    t1 = kaix_nil();
    t2 = kaix_cons(t0, t1);
    return t2;
}

int64_t kaiu_main__serve_one(void *p_evf, KaiValue * p0) {
    KaiValue *r_l_0, *t0, *t1, *r___pcs_scr, *r_$2et3$24sd, *t4, *t5, *r_srv_1, *t6, *t7, *t8, *r_$2et10$24sd, *t11, *t12, *t13, *t14, *t15, *t16, *r_$2et0$24in0, *r_$2et1$24in0, *r_$2et2$24in0, *t17, *r_$2et19$24sd, *t20, *t21, *t22, *t23, *t24, *t25;
    int64_t t3, t10, t19, t26, t27, r___pcs_arm_ret_2, t28, t29;
    int32_t t2, t9, t18;
    KaiCont r___kai_cont;
    KaiValue *r___evf = NULL;
    KaiFiber *kc_unw_fiber = NULL;
    r___evf = p_evf;
    r_l_0 = p0;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_l_0; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    t2 = ((int32_t)(t1)->variant_tag);
    r___pcs_scr = t1;
    r_$2et3$24sd = t1;
    t4 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et3$24sd, 1LL);
    t3 = 0LL;
    switch (t2) {
    case 3: goto L_L2;
    case 2: goto L_L3;
    default: goto L_L1;
    }
L_L1:;
    t5 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L2:;
    t3 = 0LL;
    goto L_L0;
L_L3:;
    r_srv_1 = kaix_variant_arg(t1, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et3$24sd, 0LL);
        kaix_arm_share(took_, r_srv_1);
    }
    r_$2et3$24sd = kaix_int(0LL);
    t6 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t7 = kaix_int(16LL);
    t8 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; KaiValue *a1_ = t7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 7))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t6);
    t9 = ((int32_t)(t8)->variant_tag);
    r___pcs_scr = t8;
    r_$2et10$24sd = t8;
    t11 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et10$24sd, 1LL);
    t10 = 0LL;
    switch (t9) {
    case 3: goto L_L6;
    case 2: goto L_L7;
    default: goto L_L5;
    }
L_L5:;
    t12 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L6:;
    t13 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t14 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t13);
    KD(t14);
    t10 = 0LL;
    goto L_L4;
L_L7:;
    t15 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t16 = kaix_unit();
    goto L_entry$24in0;
L_entry$24in0:;
    r_$2et0$24in0 = kaix_int(75LL);
    r_$2et1$24in0 = kaix_nil();
    r_$2et2$24in0 = kaix_cons(r_$2et0$24in0, r_$2et1$24in0);
    t16 = r_$2et2$24in0;
    goto L_Lj$24in0;
L_Lj$24in0:;
    t17 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; KaiValue *a1_ = t16; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 6))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t15);
    t18 = ((int32_t)(t17)->variant_tag);
    r___pcs_scr = t17;
    r_$2et19$24sd = t17;
    t20 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et19$24sd, 1LL);
    t19 = 0LL;
    switch (t18) {
    case 3: goto L_L10;
    case 2: goto L_L11;
    default: goto L_L9;
    }
L_L9:;
    t21 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L10:;
    t22 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t23 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t22);
    KD(t23);
    t19 = 0LL;
    goto L_L8;
L_L11:;
    t24 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t25 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t24);
    KD(t25);
    t19 = 1LL;
    goto L_L8;
L_L8:;
    (void)kaix_unw_pop((KaiFiber *) t20);
    KD(r_$2et19$24sd);
    t26 = t19;
    t10 = t26;
    goto L_L4;
L_L4:;
    (void)kaix_unw_pop((KaiFiber *) t11);
    KD(r_$2et10$24sd);
    t27 = t10;
    r___pcs_arm_ret_2 = t27;
    KD(r_srv_1);
    t28 = r___pcs_arm_ret_2;
    t3 = t28;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t4);
    KD(r_$2et3$24sd);
    t29 = t3;
    return t29;
}

int64_t kaiu_main__client_one(void *p_evf, int64_t p0) {
    KaiValue *t0, *t1, *t2, *r___pcs_scr, *r_$2et4$24sd, *t5, *t6, *r_c_1, *t7, *t8, *r_$2et0$24in0, *r_$2et1$24in0, *r_$2et2$24in0, *t9, *r_$2et11$24sd, *t12, *t13, *t14, *t15, *t16, *t17, *t18, *r_$2et20$24sd, *t21, *t22, *t23, *t24, *t25, *t26;
    int64_t r_port_0, t4, t11, t20, t27, t28, r___pcs_arm_ret_2, t29, t30;
    int32_t t3, t10, t19;
    KaiCont r___kai_cont;
    KaiValue *r___evf = NULL;
    KaiFiber *kc_unw_fiber = NULL;
    r___evf = p_evf;
    r_port_0 = p0;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int(r_port_0);
    t2 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = kaix_str_n("127.0.0.1", 9LL); KaiValue *a1_ = t1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    t3 = ((int32_t)(t2)->variant_tag);
    r___pcs_scr = t2;
    r_$2et4$24sd = t2;
    t5 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et4$24sd, 1LL);
    t4 = 0LL;
    switch (t3) {
    case 3: goto L_L2;
    case 2: goto L_L3;
    default: goto L_L1;
    }
L_L1:;
    t6 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L2:;
    t4 = 0LL;
    goto L_L0;
L_L3:;
    r_c_1 = kaix_variant_arg(t2, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et4$24sd, 0LL);
        kaix_arm_share(took_, r_c_1);
    }
    r_$2et4$24sd = kaix_int(0LL);
    t7 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t8 = kaix_unit();
    goto L_entry$24in0;
L_entry$24in0:;
    r_$2et0$24in0 = kaix_int(80LL);
    r_$2et1$24in0 = kaix_nil();
    r_$2et2$24in0 = kaix_cons(r_$2et0$24in0, r_$2et1$24in0);
    t8 = r_$2et2$24in0;
    goto L_Lj$24in0;
L_Lj$24in0:;
    t9 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; KaiValue *a1_ = t8; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 6))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t7);
    t10 = ((int32_t)(t9)->variant_tag);
    r___pcs_scr = t9;
    r_$2et11$24sd = t9;
    t12 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et11$24sd, 1LL);
    t11 = 0LL;
    switch (t10) {
    case 3: goto L_L6;
    case 2: goto L_L7;
    default: goto L_L5;
    }
L_L5:;
    t13 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L6:;
    t14 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t15 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t14);
    KD(t15);
    t11 = 0LL;
    goto L_L4;
L_L7:;
    t16 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t17 = kaix_int(16LL);
    t18 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; KaiValue *a1_ = t17; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 7))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t16);
    t19 = ((int32_t)(t18)->variant_tag);
    r___pcs_scr = t18;
    r_$2et20$24sd = t18;
    t21 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et20$24sd, 1LL);
    t20 = 0LL;
    switch (t19) {
    case 3: goto L_L10;
    case 2: goto L_L11;
    default: goto L_L9;
    }
L_L9:;
    t22 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L10:;
    t23 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t24 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t23);
    KD(t24);
    t20 = 0LL;
    goto L_L8;
L_L11:;
    t25 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t26 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t25);
    KD(t26);
    t20 = 1LL;
    goto L_L8;
L_L8:;
    (void)kaix_unw_pop((KaiFiber *) t21);
    KD(r_$2et20$24sd);
    t27 = t20;
    t11 = t27;
    goto L_L4;
L_L4:;
    (void)kaix_unw_pop((KaiFiber *) t12);
    KD(r_$2et11$24sd);
    t28 = t11;
    r___pcs_arm_ret_2 = t28;
    KD(r_c_1);
    t29 = r___pcs_arm_ret_2;
    t4 = t29;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t5);
    KD(r_$2et4$24sd);
    t30 = t4;
    return t30;
}

KaiValue * kai_main(void) {
    KaiValue *t0, *t1, *t2, *r___pcs_scr, *r_$2et4$24sd, *t5, *t4, *t6, *r_e_0, *t7, *t8, *t9, *r_l_1, *t10, *t11, *r_$2et0$24in0, *r_port_2, *t13, *t14, *t15, *r_accepted_3, *t16, *t17, *t18, *t19, *t20, *t21, *t22, *t23, *t24, *t25, *t26, *t27, *t28, *r___pcs_tret_4, *t29;
    int64_t t12, r_$2et1$24in0;
    int32_t t3;
    KaiCont r___kai_cont;
    KaiFiber *kc_unw_fiber = NULL;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int(0LL);
    t2 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("NetTcp"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_NetTcp), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = kaix_str_n("127.0.0.1", 9LL); KaiValue *a1_ = t1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    t3 = ((int32_t)(t2)->variant_tag);
    r___pcs_scr = t2;
    r_$2et4$24sd = t2;
    t5 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et4$24sd, 1LL);
    t4 = kaix_unit();
    switch (t3) {
    case 3: goto L_L2;
    case 2: goto L_L3;
    default: goto L_L1;
    }
L_L1:;
    t6 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L2:;
    r_e_0 = kaix_variant_arg(t2, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et4$24sd, 0LL);
        kaix_arm_share(took_, r_e_0);
    }
    r_$2et4$24sd = kaix_int(0LL);
    t7 = kaix_core_string_concat(kaix_str_n("listen FAIL: ", 13LL), r_e_0);
    t8 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    KD(t8);
    t9 = kaix_int(1LL);
    t4 = t9;
    goto L_L0;
L_L3:;
    r_l_1 = kaix_variant_arg(t2, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et4$24sd, 0LL);
        kaix_arm_share(took_, r_l_1);
    }
    r_$2et4$24sd = kaix_int(0LL);
    t10 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_l_1 }), 1LL);
    t11 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = kaix_str_n("listen ok", 9LL); void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t10);
    KD(t11);
    KU(r_l_1);
    t12 = 0LL;
    goto L_entry$24in0;
L_entry$24in0:;
    r_$2et0$24in0 = kaix_field_at(r_l_1, (int32_t)1LL);
    KD(r_l_1);
    r_$2et1$24in0 = kaix_int_field(r_$2et0$24in0);
    KD(r_$2et0$24in0);
    t12 = r_$2et1$24in0;
    goto L_Lj$24in0;
L_Lj$24in0:;
    r_port_2 = kaix_int(t12);
    t13 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_port_2, r_l_1 }), 2LL);
    t14 = kaix_closure((KaiFn)_kaiu_lam_main_103_30, (int32_t)0LL, (int32_t)2LL, (KaiValue *[]){r_port_2, r_l_1});
    t15 = kaiu_spawn__nursery(t14);
    (void)kaix_unw_pop((KaiFiber *) t13);
    r_accepted_3 = t15;
    t16 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_accepted_3, r_port_2, r_l_1 }), 3LL);
    KU(r_accepted_3);
    t17 = kaix_core_int_to_string(r_accepted_3);
    t18 = kaix_core_string_concat(kaix_str_n("accepted ", 9LL), t17);
    t19 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t18; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t16);
    KD(t19);
    KD(r_port_2);
    KU(r_accepted_3);
    t20 = kaix_int(3LL);
    t21 = kaix_eq(r_accepted_3, t20);
    t22 = kaix_unit();
    if (kaix_truthy(t21)) goto L_L4; else goto L_L5;
L_L4:;
    t23 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_accepted_3, r_l_1 }), 2LL);
    t24 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = kaix_str_n("ok", 2LL); void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t23);
    KD(t24);
    t25 = kaix_int(0LL);
    t22 = t25;
    goto L_L6;
L_L5:;
    t26 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_accepted_3, r_l_1 }), 2LL);
    t27 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = kaix_str_n("fail", 4LL); void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t26);
    KD(t27);
    t28 = kaix_int(1LL);
    t22 = t28;
    goto L_L6;
L_L6:;
    r___pcs_tret_4 = t22;
    KD(r_accepted_3);
    KD(r_l_1);
    t29 = r___pcs_tret_4;
    t4 = t29;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t5);
    KD(r_$2et4$24sd);
    return t4;
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_109_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_2, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_2 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_2);
    t2 = kaiu_main__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_108_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_2, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_2 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_2);
    t2 = kaiu_main__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_107_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_2, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_2 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_2);
    t2 = kaiu_main__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_106_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_1, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_1);
    t1 = kaiu_main__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_1);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_105_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_1, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_1);
    t1 = kaiu_main__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_1);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam__kaiu_lam_main_103_30_104_26(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_1, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_1);
    t1 = kaiu_main__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_1);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam_main_103_30(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_2, *r_l_1, *t0, *t1, *t2, *r_s1_5, *t3, *t4, *t5, *r_s2_6, *t6, *t7, *t8, *r_s3_7, *t9, *t10, *t11, *r_c1_8, *t12, *t13, *t14, *r_c2_9, *t15, *t16, *t17, *r_c3_10, *t18, *t19, *t21, *t22, *t25, *t26, *t29, *t30, *t32, *t33, *t36;
    int64_t t20, t23, t24, t27, t28, r_r1_11, t31, t34, t35, t37, t38, r_r2_12, t42;
    int32_t t39, t40, t41;
    KaiCont r___kai_cont;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_2 = kaix_capture(p_self, 0);
    r_l_1 = kaix_capture(p_self, 1);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_104_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_1});
    t2 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    r_s1_5 = t2;
    t3 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s1_5 }), 1LL);
    t4 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_105_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_1});
    t5 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t4; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t3);
    r_s2_6 = t5;
    t6 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s2_6, r_s1_5 }), 2LL);
    t7 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_106_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_1});
    t8 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t6);
    r_s3_7 = t8;
    t9 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s3_7, r_s2_6, r_s1_5 }), 3LL);
    t10 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_107_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_2});
    t11 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t10; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t9);
    r_c1_8 = t11;
    t12 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c1_8, r_s3_7, r_s2_6, r_s1_5 }), 4LL);
    t13 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_108_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_2});
    t14 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t13; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t12);
    r_c2_9 = t14;
    t15 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c2_9, r_c1_8, r_s3_7, r_s2_6, r_s1_5 }), 5LL);
    t16 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_main_103_30_109_26, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_2});
    t17 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t16; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t15);
    r_c3_10 = t17;
    t18 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_10, r_c2_9, r_c1_8, r_s3_7, r_s2_6 }), 5LL);
    t19 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s1_5; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t18);
    t20 = kaix_int_field(t19);
    KD(t19);
    t21 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_10, r_c2_9, r_c1_8, r_s3_7 }), 4LL);
    t22 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s2_6; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t21);
    t23 = kaix_int_field(t22);
    KD(t22);
    t24 = ((int64_t)((uint64_t)t20 + (uint64_t)t23));
    t25 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_10, r_c2_9, r_c1_8 }), 3LL);
    t26 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s3_7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t25);
    t27 = kaix_int_field(t26);
    KD(t26);
    t28 = ((int64_t)((uint64_t)t24 + (uint64_t)t27));
    r_r1_11 = t28;
    t29 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_10, r_c2_9 }), 2LL);
    t30 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c1_8; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t29);
    t31 = kaix_int_field(t30);
    KD(t30);
    t32 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_10 }), 1LL);
    t33 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c2_9; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t32);
    t34 = kaix_int_field(t33);
    KD(t33);
    t35 = ((int64_t)((uint64_t)t31 + (uint64_t)t34));
    t36 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c3_10; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    t37 = kaix_int_field(t36);
    KD(t36);
    t38 = ((int64_t)((uint64_t)t35 + (uint64_t)t37));
    r_r2_12 = t38;
    t39 = ((int32_t)(r_r1_11 == 3LL));
    t40 = ((int32_t)(r_r2_12 == 3LL));
    t41 = ((int32_t)(t39 && t40));
    t42 = 0LL;
    if (t41) goto L_L0; else goto L_L1;
L_L0:;
    t42 = 3LL;
    goto L_L2;
L_L1:;
    t42 = 0LL;
    goto L_L2;
L_L2:;
    return kaix_int(t42);
}

void kai_main_install_defaults(void) {
    {
        static const char *lbl;
        kaix_ev_set_handler_id(_kai_default_ev_Stdout, kaix_fresh_handler_id());
    kaix_ev_set_op(_kai_default_ev_Stdout, 3, (void *)kaix_default_stdout_print);
    kaix_ev_set_op(_kai_default_ev_Stdout, 4, (void *)kaix_default_stdout_is_tty);
        kaix_evidence_init_default_l((KaiEvidence *)_kai_default_node_Stdout, kaix_eff_label(&lbl, "Stdout"), _kai_default_ev_Stdout);
    }
    {
        static const char *lbl;
        kaix_ev_set_handler_id(_kai_default_ev_NetTcp, kaix_fresh_handler_id());
    kaix_ev_set_op(_kai_default_ev_NetTcp, 3, (void *)kaix_default_nettcp_connect);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 4, (void *)kaix_default_nettcp_listen);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 5, (void *)kaix_default_nettcp_accept);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 6, (void *)kaix_default_nettcp_send);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 7, (void *)kaix_default_nettcp_recv);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 8, (void *)kaix_default_nettcp_recv_timeout);
    kaix_ev_set_op(_kai_default_ev_NetTcp, 9, (void *)kaix_default_nettcp_close);
        kaix_evidence_init_default_l((KaiEvidence *)_kai_default_node_NetTcp, kaix_eff_label(&lbl, "NetTcp"), _kai_default_ev_NetTcp);
    }
    {
        static const char *lbl;
        kaix_ev_set_handler_id(_kai_default_ev_Mutable, kaix_fresh_handler_id());
    kaix_ev_set_op(_kai_default_ev_Mutable, 3, (void *)kaix_default_mutable_array_make);
    kaix_ev_set_op(_kai_default_ev_Mutable, 4, (void *)kaix_default_mutable_array_length);
    kaix_ev_set_op(_kai_default_ev_Mutable, 5, (void *)kaix_default_mutable_array_get);
    kaix_ev_set_op(_kai_default_ev_Mutable, 6, (void *)kaix_default_mutable_array_set);
    kaix_ev_set_op(_kai_default_ev_Mutable, 7, (void *)kaix_default_mutable_array_grow);
    kaix_ev_set_op(_kai_default_ev_Mutable, 8, (void *)kaix_default_mutable_ref_make);
    kaix_ev_set_op(_kai_default_ev_Mutable, 9, (void *)kaix_default_mutable_ref_get);
    kaix_ev_set_op(_kai_default_ev_Mutable, 10, (void *)kaix_default_mutable_ref_set);
        kaix_evidence_init_default_l((KaiEvidence *)_kai_default_node_Mutable, kaix_eff_label(&lbl, "Mutable"), _kai_default_ev_Mutable);
    }
    {
        static const char *lbl;
        kaix_ev_set_handler_id(_kai_default_ev_Spawn, kaix_fresh_handler_id());
    kaix_ev_set_op(_kai_default_ev_Spawn, 3, (void *)kaix_default_spawn_yield);
    kaix_ev_set_op(_kai_default_ev_Spawn, 4, (void *)kaix_default_spawn_spawn);
    kaix_ev_set_op(_kai_default_ev_Spawn, 5, (void *)kaix_default_spawn_await);
    kaix_ev_set_op(_kai_default_ev_Spawn, 6, (void *)kaix_default_spawn_select);
    kaix_ev_set_op(_kai_default_ev_Spawn, 7, (void *)kaix_default_spawn_cancel);
    kaix_ev_set_op(_kai_default_ev_Spawn, 8, (void *)kaix_default_spawn_set_trap_exit);
    kaix_ev_set_op(_kai_default_ev_Spawn, 9, (void *)kaix_default_spawn_scope_enter);
    kaix_ev_set_op(_kai_default_ev_Spawn, 10, (void *)kaix_default_spawn_scope_exit);
        kaix_evidence_init_default_l((KaiEvidence *)_kai_default_node_Spawn, kaix_eff_label(&lbl, "Spawn"), _kai_default_ev_Spawn);
    }
}
void kai_main_teardown_defaults(void) {
}
void _kai_proto_init_llvm(void) {
    kaix_register_one_variant_head(0, 16);
    kaix_register_one_variant_head(1, 16);
    kaix_register_one_variant_head(2, 17);
    kaix_register_one_variant_head(3, 17);
    kaix_register_one_variant_head(4, 18);
    kaix_register_one_variant_head(5, 18);
    kaix_register_one_variant_head(6, 18);
    kaix_register_one_variant_head(7, 18);
    kaix_register_one_variant_head(8, 18);
    kaix_register_one_variant_head(9, 19);
    kaix_register_one_variant_head(10, 19);
    kaix_register_one_variant_head(11, 18);
}

