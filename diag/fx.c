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
int64_t kaiu_net_tcp_shared_listener__serve_one(void *p_evf, KaiValue * p0);
int64_t kaiu_net_tcp_shared_listener__client_one(void *p_evf, int64_t p0);
KaiValue * kaiu_net_tcp_shared_listener__round(void *p_evf, KaiValue * p0, KaiValue * p1);
int64_t kaiu_net_tcp_shared_listener__rounds(void *p_evf, KaiValue * p0, int64_t p1, int64_t p2, int64_t p3);
KaiValue * kai_main(void);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_65_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_64_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_63_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_62_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_61_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_60_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_59_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_58_22(KaiValue *p_self, KaiValue **p_args, int p_n);
KaiValue *_kaiu_lam_net_tcp_shared_listener__round_57_11(KaiValue *p_self, KaiValue **p_args, int p_n);
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

int64_t kaiu_net_tcp_shared_listener__serve_one(void *p_evf, KaiValue * p0) {
    KaiValue *r_l_0, *t0, *t1, *r___pcs_scr, *r_$2et3$24sd, *t4, *t5, *r_srv_1, *t6, *t7, *t8, *r_$2et10$24sd, *t11, *t12, *t13, *t14, *t15, *t16, *t17, *t18, *t19, *r_$2et21$24sd, *t22, *t23, *t24, *t25, *t26, *t27;
    int64_t t3, t10, t21, t28, t29, r___pcs_arm_ret_2, t30, t31;
    int32_t t2, t9, t20;
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
    t16 = kaix_int(75LL);
    t17 = kaix_nil();
    t18 = kaix_cons(t16, t17);
    t19 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; KaiValue *a1_ = t18; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 6))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t15);
    t20 = ((int32_t)(t19)->variant_tag);
    r___pcs_scr = t19;
    r_$2et21$24sd = t19;
    t22 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et21$24sd, 1LL);
    t21 = 0LL;
    switch (t20) {
    case 3: goto L_L10;
    case 2: goto L_L11;
    default: goto L_L9;
    }
L_L9:;
    t23 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L10:;
    t24 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t25 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t24);
    KD(t25);
    t21 = 0LL;
    goto L_L8;
L_L11:;
    t26 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_srv_1 }), 1LL);
    KU(r_srv_1);
    t27 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_srv_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t26);
    KD(t27);
    t21 = 1LL;
    goto L_L8;
L_L8:;
    (void)kaix_unw_pop((KaiFiber *) t22);
    KD(r_$2et21$24sd);
    t28 = t21;
    t10 = t28;
    goto L_L4;
L_L4:;
    (void)kaix_unw_pop((KaiFiber *) t11);
    KD(r_$2et10$24sd);
    t29 = t10;
    r___pcs_arm_ret_2 = t29;
    KD(r_srv_1);
    t30 = r___pcs_arm_ret_2;
    t3 = t30;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t4);
    KD(r_$2et3$24sd);
    t31 = t3;
    return t31;
}

int64_t kaiu_net_tcp_shared_listener__client_one(void *p_evf, int64_t p0) {
    KaiValue *t0, *t1, *t2, *r___pcs_scr, *r_$2et4$24sd, *t5, *t6, *r_c_1, *t7, *t8, *t9, *t10, *t11, *r_$2et13$24sd, *t14, *t15, *t16, *t17, *t18, *t19, *t20, *r_$2et22$24sd, *t23, *t24, *t25, *t26, *t27, *t28;
    int64_t r_port_0, t4, t13, t22, t29, t30, r___pcs_arm_ret_2, t31, t32;
    int32_t t3, t12, t21;
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
    t8 = kaix_int(80LL);
    t9 = kaix_nil();
    t10 = kaix_cons(t8, t9);
    t11 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; KaiValue *a1_ = t10; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 6))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t7);
    t12 = ((int32_t)(t11)->variant_tag);
    r___pcs_scr = t11;
    r_$2et13$24sd = t11;
    t14 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et13$24sd, 1LL);
    t13 = 0LL;
    switch (t12) {
    case 3: goto L_L6;
    case 2: goto L_L7;
    default: goto L_L5;
    }
L_L5:;
    t15 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L6:;
    t16 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t17 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t16);
    KD(t17);
    t13 = 0LL;
    goto L_L4;
L_L7:;
    t18 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t19 = kaix_int(16LL);
    t20 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; KaiValue *a1_ = t19; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 7))(h_, a0_, a1_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t18);
    t21 = ((int32_t)(t20)->variant_tag);
    r___pcs_scr = t20;
    r_$2et22$24sd = t20;
    t23 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, &r_$2et22$24sd, 1LL);
    t22 = 0LL;
    switch (t21) {
    case 3: goto L_L10;
    case 2: goto L_L11;
    default: goto L_L9;
    }
L_L9:;
    t24 = kaix_core_panic(kaix_str_n("non-exhaustive match", 20LL));
    __builtin_unreachable();
L_L10:;
    t25 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t26 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t25);
    KD(t26);
    t22 = 0LL;
    goto L_L8;
L_L11:;
    t27 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c_1 }), 1LL);
    KU(r_c_1);
    t28 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); kaix_cancel_poll(fib_); void *node_ = (void *) ((void **) r___evf)[0]; void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c_1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 9))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t27);
    KD(t28);
    t22 = 1LL;
    goto L_L8;
L_L8:;
    (void)kaix_unw_pop((KaiFiber *) t23);
    KD(r_$2et22$24sd);
    t29 = t22;
    t13 = t29;
    goto L_L4;
L_L4:;
    (void)kaix_unw_pop((KaiFiber *) t14);
    KD(r_$2et13$24sd);
    t30 = t13;
    r___pcs_arm_ret_2 = t30;
    KD(r_c_1);
    t31 = r___pcs_arm_ret_2;
    t4 = t31;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t5);
    KD(r_$2et4$24sd);
    t32 = t4;
    return t32;
}

KaiValue * kaiu_net_tcp_shared_listener__round(void *p_evf, KaiValue * p0, KaiValue * p1) {
    KaiValue *r_l_0, *r_port_1, *t0, *t1, *t2, *t3, *r___pcs_ret_2, *t4;
    KaiValue *r___evf = NULL;
    KaiFiber *kc_unw_fiber = NULL;
    r___evf = p_evf;
    r_l_0 = p0;
    r_port_1 = p1;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_l_0, r_port_1 }), 2LL);
    t2 = kaix_closure((KaiFn)_kaiu_lam_net_tcp_shared_listener__round_57_11, (int32_t)0LL, (int32_t)2LL, (KaiValue *[]){r_port_1, r_l_0});
    t3 = kaiu_spawn__nursery(t2);
    (void)kaix_unw_pop((KaiFiber *) t1);
    r___pcs_ret_2 = t3;
    KD(r_l_0);
    KD(r_port_1);
    t4 = r___pcs_ret_2;
    return t4;
}

int64_t kaiu_net_tcp_shared_listener__rounds(void *p_evf, KaiValue * p0, int64_t p1, int64_t p2, int64_t p3) {
    KaiValue *r_l_0, *t0, *t4, *t5, *t6;
    int64_t r_port_1, r_left_2, r_ok_3, t2, t3, r___unw_v0_5, t7, t8, r___unw_v1_6, t9, r___pcs_ret_4, t10;
    int32_t t1;
    KaiValue *r___evf = NULL;
    KaiFiber *kc_unw_fiber = NULL;
    r___evf = p_evf;
    r_l_0 = p0;
    r_port_1 = p1;
    r_left_2 = p2;
    r_ok_3 = p3;
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = ((int32_t)(r_left_2 == 0LL));
    t2 = 0LL;
    if (t1) goto L_L0; else goto L_L1;
L_L0:;
    t2 = r_ok_3;
    goto L_L2;
L_L1:;
    t3 = ((int64_t)((uint64_t)r_left_2 - (uint64_t)1LL));
    r___unw_v0_5 = t3;
    t4 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_l_0 }), 1LL);
    KU(r_l_0);
    t5 = kaix_int(r_port_1);
    t6 = kaiu_net_tcp_shared_listener__round(r___evf, r_l_0, t5);
    (void)kaix_unw_pop((KaiFiber *) t4);
    t7 = kaix_int_field(t6);
    KD(t6);
    t8 = ((int64_t)((uint64_t)r_ok_3 + (uint64_t)t7));
    r___unw_v1_6 = t8;
    KU(r_l_0);
    { KaiValue * t0_ = r_l_0; int64_t t1_ = r_port_1; int64_t t2_ = r___unw_v0_5; int64_t t3_ = r___unw_v1_6; KD(r_l_0); r_l_0 = t0_; r_port_1 = t1_; r_left_2 = t2_; r_ok_3 = t3_; }
    goto L_entry;
L_L3:;
    t2 = kaix_int_field(kaix_unit());
    goto L_L2;
L_L2:;
    t9 = t2;
    r___pcs_ret_4 = t9;
    KD(r_l_0);
    t10 = r___pcs_ret_4;
    return t10;
}

KaiValue * kai_main(void) {
    KaiValue *t0, *t1, *t2, *r___pcs_scr, *r_$2et4$24sd, *t5, *t6, *r_e_0, *t7, *t8, *r_l_1, *t9, *r_$2et0$24in0, *t12, *t13, *t14, *t15, *t16;
    int64_t t4, t10, r_$2et1$24in0, t11, r_total_2, t18, t19;
    int32_t t17;
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
    r_e_0 = kaix_variant_arg(t2, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et4$24sd, 0LL);
        kaix_arm_share(took_, r_e_0);
    }
    r_$2et4$24sd = kaix_int(0LL);
    t7 = kaix_core_string_concat(kaix_str_n("listen FAIL: ", 13LL), r_e_0);
    t8 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    KD(t8);
    t4 = 1LL;
    goto L_L0;
L_L3:;
    r_l_1 = kaix_variant_arg(t2, 0);
    {
        int32_t took_ = kaix_arm_take(r_$2et4$24sd, 0LL);
        kaix_arm_share(took_, r_l_1);
    }
    r_$2et4$24sd = kaix_int(0LL);
    t9 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_l_1 }), 1LL);
    KU(r_l_1);
    KU(r_l_1);
    t10 = 0LL;
    goto L_entry$24in0;
L_entry$24in0:;
    r_$2et0$24in0 = kaix_field_at(r_l_1, (int32_t)1LL);
    KD(r_l_1);
    r_$2et1$24in0 = kaix_int_field(r_$2et0$24in0);
    KD(r_$2et0$24in0);
    t10 = r_$2et1$24in0;
    goto L_Lj$24in0;
L_Lj$24in0:;
    t11 = kaiu_net_tcp_shared_listener__rounds(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_1, t10, 150LL, 0LL);
    (void)kaix_unw_pop((KaiFiber *) t9);
    r_total_2 = t11;
    t12 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_l_1 }), 1LL);
    t13 = kaix_int(r_total_2);
    t14 = kaix_core_int_to_string(t13);
    t15 = kaix_core_string_concat(kaix_str_n("completed ", 10LL), t14);
    t16 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Stdout"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Stdout), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t15; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 3))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t12);
    KD(t16);
    KD(r_l_1);
    t17 = ((int32_t)(r_total_2 == 1200LL));
    t18 = 0LL;
    if (t17) goto L_L4; else goto L_L5;
L_L4:;
    t18 = 0LL;
    goto L_L6;
L_L5:;
    t18 = 1LL;
    goto L_L6;
L_L6:;
    t19 = t18;
    t4 = t19;
    goto L_L0;
L_L0:;
    (void)kaix_unw_pop((KaiFiber *) t5);
    KD(r_$2et4$24sd);
    return kaix_int(t4);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_65_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_1, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_1);
    t2 = kaiu_net_tcp_shared_listener__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_64_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_1, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_1);
    t2 = kaiu_net_tcp_shared_listener__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_63_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_1, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_1);
    t2 = kaiu_net_tcp_shared_listener__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_62_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_1, *t0;
    int64_t t1, t2;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_1 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_int_field(r_port_1);
    t2 = kaiu_net_tcp_shared_listener__client_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), t1);
    return kaix_int(t2);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_61_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_0, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_0 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_0);
    t1 = kaiu_net_tcp_shared_listener__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_0);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_60_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_0, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_0 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_0);
    t1 = kaiu_net_tcp_shared_listener__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_0);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_59_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_0, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_0 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_0);
    t1 = kaiu_net_tcp_shared_listener__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_0);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_58_22(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_l_0, *t0;
    int64_t t1;
    KaiFiber *kc_unw_fiber = NULL;
    r_l_0 = kaix_capture(p_self, 0);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    KU(r_l_0);
    t1 = kaiu_net_tcp_shared_listener__serve_one(kaix_evidence_fill_frame_l((void *)(KaiValue *[1]){0}, 1LL, (const char *const[]){"NetTcp"}, ({ static const char *lc_[1]; lc_; }), (KaiEvidence *const[]){(KaiEvidence *)&_kai_default_node_NetTcp}, 0LL), r_l_0);
    return kaix_int(t1);
}

KaiValue *_kaiu_lam_net_tcp_shared_listener__round_57_11(KaiValue *p_self, KaiValue **p_args, int p_n) {
    KaiValue *r_port_1, *r_l_0, *t0, *t1, *t2, *r_s1_3, *t3, *t4, *t5, *r_s2_4, *t6, *t7, *t8, *r_s3_5, *t9, *t10, *t11, *r_s4_6, *t12, *t13, *t14, *r_c1_7, *t15, *t16, *t17, *r_c2_8, *t18, *t19, *t20, *r_c3_9, *t21, *t22, *t23, *r_c4_10, *t24, *t25, *t27, *t28, *t31, *t32, *t35, *t36, *t39, *t40, *t43, *t44, *t47, *t48, *t51, *t54;
    int64_t t26, t29, t30, t33, t34, t37, t38, t41, t42, t45, t46, t49, t50, t52, t53;
    KaiCont r___kai_cont;
    KaiFiber *kc_unw_fiber = NULL;
    r_port_1 = kaix_capture(p_self, 0);
    r_l_0 = kaix_capture(p_self, 1);
L_entry:;
    t0 = (KaiValue *) &kc_unw_fiber;
    t1 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_58_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_0});
    t2 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t1; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    r_s1_3 = t2;
    t3 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s1_3 }), 1LL);
    t4 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_59_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_0});
    t5 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t4; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t3);
    r_s2_4 = t5;
    t6 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s2_4, r_s1_3 }), 2LL);
    t7 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_60_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_0});
    t8 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t6);
    r_s3_5 = t8;
    t9 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s3_5, r_s2_4, r_s1_3 }), 3LL);
    t10 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_61_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_l_0});
    t11 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t10; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t9);
    r_s4_6 = t11;
    t12 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_s4_6, r_s3_5, r_s2_4, r_s1_3 }), 4LL);
    t13 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_62_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_1});
    t14 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t13; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t12);
    r_c1_7 = t14;
    t15 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c1_7, r_s4_6, r_s3_5, r_s2_4, r_s1_3 }), 5LL);
    t16 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_63_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_1});
    t17 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t16; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t15);
    r_c2_8 = t17;
    t18 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c2_8, r_c1_7, r_s4_6, r_s3_5, r_s2_4, r_s1_3 }), 6LL);
    t19 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_64_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_1});
    t20 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t19; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t18);
    r_c3_9 = t20;
    t21 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c3_9, r_c2_8, r_c1_7, r_s4_6, r_s3_5, r_s2_4, r_s1_3 }), 7LL);
    t22 = kaix_closure((KaiFn)_kaiu_lam__kaiu_lam_net_tcp_shared_listener__round_57_11_65_22, (int32_t)0LL, (int32_t)1LL, (KaiValue *[]){r_port_1});
    t23 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = t22; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 4))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t21);
    r_c4_10 = t23;
    t24 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9, r_c2_8, r_c1_7, r_s4_6, r_s3_5, r_s2_4 }), 7LL);
    t25 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s1_3; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t24);
    t26 = kaix_int_field(t25);
    KD(t25);
    t27 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9, r_c2_8, r_c1_7, r_s4_6, r_s3_5 }), 6LL);
    t28 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s2_4; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t27);
    t29 = kaix_int_field(t28);
    KD(t28);
    t30 = ((int64_t)((uint64_t)t26 + (uint64_t)t29));
    t31 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9, r_c2_8, r_c1_7, r_s4_6 }), 5LL);
    t32 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s3_5; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t31);
    t33 = kaix_int_field(t32);
    KD(t32);
    t34 = ((int64_t)((uint64_t)t30 + (uint64_t)t33));
    t35 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9, r_c2_8, r_c1_7 }), 4LL);
    t36 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_s4_6; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t35);
    t37 = kaix_int_field(t36);
    KD(t36);
    t38 = ((int64_t)((uint64_t)t34 + (uint64_t)t37));
    t39 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9, r_c2_8 }), 3LL);
    t40 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c1_7; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t39);
    t41 = kaix_int_field(t40);
    KD(t40);
    t42 = ((int64_t)((uint64_t)t38 + (uint64_t)t41));
    t43 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10, r_c3_9 }), 2LL);
    t44 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c2_8; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t43);
    t45 = kaix_int_field(t44);
    KD(t44);
    t46 = ((int64_t)((uint64_t)t42 + (uint64_t)t45));
    t47 = (KaiValue *) kaix_unw_push_f((KaiFiber **) t0, ((KaiValue *[]){ r_c4_10 }), 1LL);
    t48 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c3_9; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    (void)kaix_unw_pop((KaiFiber *) t47);
    t49 = kaix_int_field(t48);
    KD(t48);
    t50 = ((int64_t)((uint64_t)t46 + (uint64_t)t49));
    t51 = ({ KaiFiber *fib_ = kaix_dispatch_fiber(); const char *eff_ = KAI_EFF("Spawn"); void *node_ = kaix_evidence_require(kaix_evidence_lookup_or_default_l(eff_, (KaiEvidence *) _kai_default_node_Spawn), eff_); void *h_ = kaix_evidence_node_handler(node_); kaix_cont_init_identity(&r___kai_cont, (KaiHandlerId) kaix_ev_handler_id(h_)); KaiValue *a0_ = r_c4_10; void *prev_ = kaix_disp_enter(fib_, node_); KaiValue *r_ = ((KaiValue *(*)(void *, KaiValue *, KaiCont *)) kaix_ev_op_at(h_, 5))(h_, a0_, &r___kai_cont); kaix_disp_leave(fib_, prev_); kaix_op_finish(node_, &r___kai_cont, r_); });
    t52 = kaix_int_field(t51);
    KD(t51);
    t53 = ((int64_t)((uint64_t)t50 + (uint64_t)t52));
    t54 = kaix_int(t53);
    return t54;
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

