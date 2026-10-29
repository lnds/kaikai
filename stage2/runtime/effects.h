/* Fragment of runtime.h; include runtime.h, never this file. */
/* =================================================================
 * Effects: handler-stack runtime (m7a #5)
 * =================================================================
 *
 * Per docs/effects-impl.md §*Handler-stack runtime*. Each fiber
 * owns a stack of Evidence nodes; m7a operates with a single
 * implicit fiber (the root fiber), but the layout is per-fiber so
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

/* Handler ids must be unique per process: a handler installed in one TU and
 * one in another would draw colliding ids from a per-TU counter (both start
 * at 1). One shared counter (owner defines, others extern). Under M:N every
 * scheduler thread installs handlers, so the bump is atomic — colliding ids
 * across threads would alias distinct handler frames in effect dispatch. */
#if defined(KAI_SEPARATE_COMPILATION)
extern _Atomic KaiHandlerId kai_next_handler_id;
#  if defined(KAI_RUNTIME_OWNER)
_Atomic KaiHandlerId kai_next_handler_id = 1;
#  endif
#else
static _Atomic KaiHandlerId kai_next_handler_id = 1;
#endif

static KaiHandlerId kai_fresh_handler_id(void) {
    return atomic_fetch_add(&kai_next_handler_id, 1);
}

/* The one-shot continuation an op call site passes its clause, on the
 * call site's stack. The clause runs above the op site, so resuming is
 * returning the value: `status` alone carries the one-shot check, and
 * after the clause returns it tells the op site whether the clause
 * resumed or abandoned. */
typedef enum {
    KAI_CONT_UNRESUMED = 0,
    KAI_CONT_RESUMED   = 1
} KaiContStatus;

typedef struct KaiCont KaiCont;
struct KaiCont {
    KaiContStatus  status;
    KaiHandlerId   handler_id;
};

static inline void kai_cont_init_identity(KaiCont *k, KaiHandlerId hid) {
    k->status     = KAI_CONT_UNRESUMED;
    k->handler_id = hid;
}

__attribute__((noinline, cold, noreturn)) static void kai_cont_resumed_twice(const KaiCont *k) {
    kai_eprintf(
        "kai: continuation resumed twice (handler #%llu)\n",
        (unsigned long long) k->handler_id);
    kai_exit(1);
}

/* Surface `resume(v)` lowers to this. */
static inline KaiValue *kai_cont_resume(KaiCont *k, KaiValue *v) {
    if (__builtin_expect(k->status != KAI_CONT_UNRESUMED, 0)) kai_cont_resumed_twice(k);
    k->status = KAI_CONT_RESUMED;
    return v;
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
    flockfile(stdout);
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stdout);
    }
    fputc('\n', stdout);
    funlockfile(stdout);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_stderr_eprint(void *self, KaiValue *s, KaiCont *k) {
    (void) self;
    kai_flush_stdout();
    flockfile(stderr);
    if (kai_is_ptr(s) && s->tag == KAI_STR) {
        fwrite(s->as.s.bytes, 1, s->as.s.len, stderr);
    }
    fputc('\n', stderr);
    funlockfile(stderr);
    return kai_cont_resume(k, kai_unit());
}

static KaiValue *kai_default_stdout_is_tty(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_bool(isatty(STDOUT_FILENO) != 0));
}

static KaiValue *kai_default_stderr_is_tty(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_bool(isatty(STDERR_FILENO) != 0));
}

static KaiValue *kai_default_stdin_is_tty(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_bool(isatty(STDIN_FILENO) != 0));
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
KAI_SCHED_FN KaiValue *kai_default_stdin_read_line(void *self, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR || KAI_HOST_JS
    kai_reactor_init();
    kai_reactor_stdin_set_nonblocking();
#endif

    char *buf = NULL;
    size_t n = 0, cap = 0;

    for (;;) {
        ssize_t r = 0;
        int st = kai_stdin_take_line(&buf, &n, &cap, &r);
        if (st >= 0) {
            if (st == 0) continue;
            KaiValue *s = kai_str_from_bytes(buf, n);
            free(buf);
            return kai_cont_resume(k, kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = s}}));
        }
        if (r == 0) {
            /* EOF: a partial line is returned, nothing buffered is None. */
            if (n == 0) {
                free(buf);
                return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
            }
            KaiValue *s = kai_str_from_bytes(buf, n);
            free(buf);
            return kai_cont_resume(k, kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = s}}));
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (kai_reactor_park_stdin(kai_current_fiber()) != 0) {
                kai_eprintf(
                    "kai: stdin: multiple fibers reading concurrently "
                    "is undefined; serialize via an actor\n");
                kai_exit(1);
            }
        } else if (errno == EINTR) {
            continue;
        } else {
            /* Real I/O error reads as end of stream. */
            free(buf);
            return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
        }
    }
}
#endif

/* Issue #453 + #620 — Phase R3: default Stdin.read_bytes handler.
 * Returns a String of at most `n` raw bytes; on EOF the returned
 * String is shorter than `n` — possibly empty. No Result wrapper —
 * the LSP framing use case treats a short read as end-of-stream.
 *
 * R3 wiring: same shape as read_line — non-blocking read() loop
 * with reactor parking on EAGAIN. The buffer is sized once up
 * front so partial reads accumulate without realloc. */
KAI_SCHED_FN KaiValue *kai_default_stdin_read_bytes(void *self, KaiValue *n, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int64_t want = 0;
    if (n && kai_is_int(n) && kai_intf(n) > 0) want = kai_intf(n);
    if (want <= 0) {
        return kai_cont_resume(k, kai_str_from_bytes("", 0));
    }

#if KAI_HOST_REACTOR || KAI_HOST_JS
    kai_reactor_init();
    kai_reactor_stdin_set_nonblocking();
#endif

    char *buf = (char *) malloc((size_t) want);
    if (!buf) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    size_t got = kai_stdin_drain(buf, (size_t) want);

    while (got < (size_t) want) {
        ssize_t r = read(STDIN_FILENO, buf + got, (size_t) want - got);
        if (r > 0) {
            got += (size_t) r;
        } else if (r == 0) {
            /* EOF — return what we have. */
            break;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (kai_reactor_park_stdin(kai_current_fiber()) != 0) {
                kai_eprintf(
                    "kai: stdin: multiple fibers reading concurrently "
                    "is undefined; serialize via an actor\n");
                kai_exit(1);
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
#endif

/* m7a #7: default Env handlers. `args()` reuses kai_core_args
 * (returns a [String] of argv[1..]); `var(name)` wraps getenv:
 * present → Some(value), absent → None. */
static KaiValue *kai_default_env_args(void *self, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_args());
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
    KaiValue *s = kai_str_dyn(got);
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
     * shape mirrors how `kai_core_args` walks argv. */
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

/* m7a #7: default File handlers. Each reuses the core helper —
 * which already produces `Result[T, String]` shapes per Doc B
 * §`File` error model — but routes the call through the Phase R1
 * reactor's file-pool worker (issue #611) so the blocking `fopen`
 * / `fread` / `fwrite` syscall runs off the scheduler thread.
 * Other fibers therefore make forward progress while one is mid
 * file op.
 *
 * Each `_arg` struct lives on the calling fiber's stack for the
 * lifetime of the work; the `_thunk` runs on a pool worker thread
 * and returns the KaiValue * the core produced. */
typedef struct {
    KaiValue *path;
} KaiFileReadFileArg;
static KaiValue *_kai_file_read_file_thunk(void *arg) {
    KaiFileReadFileArg *a = (KaiFileReadFileArg *) arg;
    return kai_core_read_file(a->path);
}
KAI_SCHED_FN KaiValue *kai_default_file_read_file(void *self, KaiValue *path, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileReadFileArg a = { kai_incref(path) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_read_file_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

typedef struct {
    KaiValue *path;
    KaiValue *contents;
} KaiFileWriteFileArg;
static KaiValue *_kai_file_write_file_thunk(void *arg) {
    KaiFileWriteFileArg *a = (KaiFileWriteFileArg *) arg;
    return kai_core_write_file(a->path, a->contents);
}
KAI_SCHED_FN KaiValue *kai_default_file_write_file(void *self, KaiValue *path,
                                              KaiValue *contents, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileWriteFileArg a = { kai_incref(path), kai_incref(contents) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_write_file_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

/* The `_thunk`s below reach `kai_core_*` helpers that consume their
 * arguments, so each op arg is increfed into the `_arg` struct: the ref
 * the call site handed over is the one the extern-handler bridge
 * releases (see the op-argument ABI note at `kai_default_spawn_yield`).
 *
 * Issue #771 Phase 1: default handlers for the five chunked `File`
 * ops. Each offloads its blocking syscall to the R1 pool worker and
 * parks the fiber, identical to read_file/write_file above — regular
 * files are not epoll-pollable, so the pool is the readiness path. The
 * `_arg` struct lives on the fiber stack for the duration of the work;
 * the `_thunk` runs on a pool thread and returns the core's value. */
typedef struct { KaiValue *path; } KaiFileOpenArg;
static KaiValue *_kai_file_open_read_thunk(void *arg) {
    return kai_core_file_open_read(((KaiFileOpenArg *) arg)->path);
}
KAI_SCHED_FN KaiValue *kai_default_file_open_read(void *self, KaiValue *path, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileOpenArg a = { kai_incref(path) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_open_read_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

typedef struct { KaiValue *h; KaiValue *max; } KaiFileReadChunkArg;
static KaiValue *_kai_file_read_chunk_thunk(void *arg) {
    KaiFileReadChunkArg *a = (KaiFileReadChunkArg *) arg;
    return kai_core_file_read_chunk(a->h, a->max);
}
KAI_SCHED_FN KaiValue *kai_default_file_read_chunk(void *self, KaiValue *h, KaiValue *max, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileReadChunkArg a = { kai_incref(h), kai_incref(max) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_read_chunk_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

static KaiValue *_kai_file_open_write_thunk(void *arg) {
    return kai_core_file_open_write(((KaiFileOpenArg *) arg)->path);
}
KAI_SCHED_FN KaiValue *kai_default_file_open_write(void *self, KaiValue *path, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileOpenArg a = { kai_incref(path) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_open_write_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

typedef struct { KaiValue *h; KaiValue *data; } KaiFileWriteChunkArg;
static KaiValue *_kai_file_write_chunk_thunk(void *arg) {
    KaiFileWriteChunkArg *a = (KaiFileWriteChunkArg *) arg;
    return kai_core_file_write_chunk(a->h, a->data);
}
KAI_SCHED_FN KaiValue *kai_default_file_write_chunk(void *self, KaiValue *h, KaiValue *data, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileWriteChunkArg a = { kai_incref(h), kai_incref(data) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_write_chunk_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

typedef struct { KaiValue *h; } KaiFileCloseArg;
static KaiValue *_kai_file_close_thunk(void *arg) {
    return kai_core_file_close(((KaiFileCloseArg *) arg)->h);
}
KAI_SCHED_FN KaiValue *kai_default_file_close_file(void *self, KaiValue *h, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
#if KAI_HOST_REACTOR
    kai_reactor_init();
#endif
    KaiFileCloseArg a = { kai_incref(h) };
    KaiValue *r = kai_reactor_run_in_pool(_kai_file_close_thunk, &a);
    return kai_cont_resume(k, r);
}
#endif

/* m7b #2b: default Mutable handler — wraps the core `array_*`
 * helpers and resumes with the result. Doc B §`Mutable` *Default
 * handler* says the trivial wrapping default is installed for any
 * main row that mentions Mutable. The op signatures here mirror
 * the core's Array[T] return shape (see compiler.kai
 * builtin_mutable_decl for the divergence note from Doc B).
 *
 * The `kai_core_*` helpers consume their arguments, so each op arg is
 * increfed into the call: the ref the call site handed over is the one
 * the extern-handler bridge releases (see the op-argument ABI note at
 * `kai_default_spawn_yield`). */
static KaiValue *kai_default_mutable_array_make(void *self, KaiValue *n,
                                                 KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_array_make(kai_incref(n), kai_incref(init)));
}

static KaiValue *kai_default_mutable_array_length(void *self, KaiValue *a, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_array_length(kai_incref(a)));
}

static KaiValue *kai_default_mutable_array_get(void *self, KaiValue *a,
                                                KaiValue *i, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_array_get(kai_incref(a), kai_incref(i)));
}

static KaiValue *kai_default_mutable_array_set(void *self, KaiValue *a,
                                                KaiValue *i, KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_array_set(kai_incref(a), kai_incref(i), kai_incref(v)));
}

static KaiValue *kai_default_mutable_array_grow(void *self, KaiValue *a,
                                                 KaiValue *n, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_array_grow(kai_incref(a), kai_incref(n), kai_incref(init)));
}

/* Issue #257: Ref[T] ops in the default Mutable handler. Each
 * trampolines to its core helper and resumes with the result,
 * mirroring the array_* clauses above. */
static KaiValue *kai_default_mutable_ref_make(void *self, KaiValue *init, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_ref_make(kai_incref(init)));
}

static KaiValue *kai_default_mutable_ref_get(void *self, KaiValue *r, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_ref_get(kai_incref(r)));
}

static KaiValue *kai_default_mutable_ref_set(void *self, KaiValue *r,
                                              KaiValue *v, KaiCont *k) {
    (void) self;
    return kai_cont_resume(k, kai_core_ref_set(kai_incref(r), kai_incref(v)));
}

/* Default Random handler — PCG32 (M.E.O'Neill 2014). One stream per
 * thread, each seeded on first use from the OS CSPRNG, so runs and
 * threads never replay one another. Non-cryptographic by design — for
 * games, simulations, sampling, fixtures. */
static void _kai_securerandom_fill(unsigned char *buf, size_t n);
/* Per-thread RNG stream: one state so a seed in one TU is observed
 * by random() in another (owner defines, others extern). */
#if defined(KAI_SEPARATE_COMPILATION)
extern KAI_TLS uint64_t _kai_pcg_state;
extern KAI_TLS uint64_t _kai_pcg_inc;
extern KAI_TLS int      _kai_pcg_seeded;
#  if defined(KAI_RUNTIME_OWNER)
KAI_TLS uint64_t _kai_pcg_state  = 0x853c49e6748fea9bULL;
KAI_TLS uint64_t _kai_pcg_inc    = 0xda3e39cb94b95bdbULL;
KAI_TLS int      _kai_pcg_seeded = 0;
#  endif
#else
static KAI_TLS uint64_t _kai_pcg_state  = 0x853c49e6748fea9bULL;
static KAI_TLS uint64_t _kai_pcg_inc    = 0xda3e39cb94b95bdbULL;
static KAI_TLS int      _kai_pcg_seeded = 0;
#endif

/* The generator's state is thread-local: each step stays inside one
 * activation that never switches context. */
__attribute__((noinline))
static uint32_t _kai_pcg32_next(void) {
    uint64_t old = _kai_pcg_state;
    _kai_pcg_state = old * 6364136223846793005ULL + (_kai_pcg_inc | 1ULL);
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << (((uint32_t)(-(int32_t)rot)) & 31u));
}

__attribute__((noinline))
static void _kai_pcg32_seed(uint64_t initstate, uint64_t initseq) {
    _kai_pcg_state = 0u;
    _kai_pcg_inc   = (initseq << 1u) | 1u;
    (void) _kai_pcg32_next();
    _kai_pcg_state += initstate;
    (void) _kai_pcg32_next();
}

__attribute__((noinline))
static void _kai_pcg32_ensure_seeded(void) {
    if (!_kai_pcg_seeded) {
        uint64_t seed[2];
        _kai_securerandom_fill((unsigned char *) seed, sizeof seed);
        _kai_pcg32_seed(seed[0], seed[1]);
        _kai_pcg_seeded = 1;
    }
}

/* int_range(lo, hi) returns a value in [lo, hi] (inclusive). lo > hi
 * is a panic. The draw reduces modulo the span; for spans well under
 * 2^32 the modulo bias is negligible. */
static KaiValue *kai_default_random_int_range(void *self, KaiValue *lo, KaiValue *hi, KaiCont *k) {
    (void) self;
    int64_t lo_v = (kai_is_int(lo)) ? kai_intf(lo) : 0;
    int64_t hi_v = (kai_is_int(hi)) ? kai_intf(hi) : 0;
    if (lo_v > hi_v) {
        kai_eprintf("kai: Random.int_range: lo (%lld) > hi (%lld)\n",
                (long long) lo_v, (long long) hi_v);
        kai_exit(1);
    }
    _kai_pcg32_ensure_seeded();
    uint64_t span = (uint64_t) (hi_v - lo_v) + 1ULL;
    uint64_t draw;
    if (span != 0 && span <= 0xFFFFFFFFULL) {
        draw = (uint64_t) _kai_pcg32_next() % span;
    } else {
        uint64_t hi32 = (uint64_t) _kai_pcg32_next();
        uint64_t lo32 = (uint64_t) _kai_pcg32_next();
        draw = (hi32 << 32) | lo32;
        if (span != 0) draw %= span;
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
        kai_eprintf("kai: Clock.wall_now: clock_gettime failed: %s\n",
                strerror(errno));
        kai_exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

static KaiValue *kai_default_clock_monotonic_now(void *self, KaiCont *k) {
    (void) self;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        kai_eprintf("kai: Clock.monotonic_now: clock_gettime failed: %s\n",
                strerror(errno));
        kai_exit(1);
    }
    return kai_cont_resume(k, _kai_clock_make_record(
        (int64_t) ts.tv_sec, (int64_t) ts.tv_nsec));
}

KAI_SCHED_FN KaiValue *kai_default_clock_sleep_ns(void *self, KaiValue *ns, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int64_t ns_v = (kai_is_int(ns)) ? kai_intf(ns) : 0;
#if KAI_HOST_JS
    /* A page only runs while the program yields: even a zero sleep waits for the host. */
    if (ns_v <= 0) kai_reactor_park_timer(kai_current_fiber(), kai_reactor_now_ns());
#endif
#if defined(__wasm__) && !KAI_HOST_JS
    if (ns_v > 0) {
        struct timespec nap = { (time_t) (ns_v / 1000000000), (long) (ns_v % 1000000000) };
        while (nanosleep(&nap, &nap) != 0 && errno == EINTR) {}
    }
    return kai_cont_resume(k, kai_unit());
#else
    if (ns_v > 0) {
#if !defined(__wasm__)
        kai_reactor_init();
#endif
        uint64_t deadline = kai_reactor_now_ns() + (uint64_t) ns_v;
        /* Re-park on a spurious resume: only a reactor splice or an
         * elapsed clock ends the sleep. Cancel delivered while the fiber
         * is parked still arrives at the next yield-point hook. */
        while (kai_reactor_now_ns() < deadline) {
            kai_reactor_park_timer(kai_current_fiber(), deadline);
            if (kai_current_fiber()->reactor_fired) break;
        }
    }
    return kai_cont_resume(k, kai_unit());
#endif
}
#endif

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

