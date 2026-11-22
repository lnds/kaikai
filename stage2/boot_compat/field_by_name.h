/* By-name record field entry points, for C emitted by a boot compiler older
 * than the constant-slot codegen. Only the boot routes of tools/kaic-boot.sh
 * define KAI_BOOT_COMPAT_FIELD_BY_NAME; nothing in this tree calls these.
 * Dead once every supported boot emits slot reads. */

static KaiValue *kai_op_field(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_incref(rec->as.rec.fields[i]);
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

static KaiValue *kai_op_field_take(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return kai_op_field_take_at(rec, i);
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

static void kai_op_field_untake(KaiValue *rec, const char *name) {
    if (!kai_check_unique(rec) || rec->tag != KAI_RECORD) return;
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            kai_op_field_untake_at(rec, i);
            return;
        }
    }
}

static KaiValue *kai_op_field_borrow(KaiValue *rec, const char *name) {
    if (!rec || rec->tag != KAI_RECORD) {
        kai_eprintf("kai: field access on non-record\n"); kai_exit(1);
    }
    for (int i = 0; i < rec->as.rec.n_fields; ++i) {
        if (strcmp(rec->as.rec.names[i], name) == 0) {
            return rec->as.rec.fields[i];
        }
    }
    kai_eprintf("kai: no such field `%s`\n", name); kai_exit(1);
}

static inline KaiValue *kaix_field(KaiValue *rec, const char *name) { return kai_op_field(rec, name); }
static inline KaiValue *kaix_field_take(KaiValue *rec, const char *name) { return kai_op_field_take(rec, name); }
static inline KaiValue *kaix_field_untake(KaiValue *rec, const char *name) { kai_op_field_untake(rec, name); return kai_unit(); }
static inline KaiValue *kaix_field_borrow(KaiValue *rec, const char *name) { return kai_op_field_borrow(rec, name); }
