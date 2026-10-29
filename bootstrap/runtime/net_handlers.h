/* Fragment of runtime.h; include runtime.h, never this file. */
#if KAI_HOST_SOCKETS
static KaiValue *_kai_net_err(KaiCont *k, int saved_errno) {
    const char *msg = strerror(saved_errno);
    if (!msg) msg = "unknown error";
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

static KaiValue *_kai_net_err_msg(KaiCont *k, const char *msg) {
    KaiValue *m = kai_str(msg);
    KaiValue *err = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
    return kai_cont_resume(k, err);
}

/* Build a Conn record `{ fd }`. Static field-name strings match
 * what the kaikai-side `type Conn = { fd: Int }` declaration
 * produces from emit_record_constructor — kai_op_field reads by
 * strcmp on the name pointer's contents, so any pointer to a
 * matching cstring works. */
static KaiValue *_kai_net_make_conn(int fd) {
    KaiValue *fd_kv = kai_int((int64_t) fd);
    KaiValue *fields[1] = { fd_kv };
    static const char *names[1] = { "fd" };
    return kai_record(1, fields, names);
}

static KaiValue *_kai_net_make_listener(int fd, int port) {
    KaiValue *fd_kv   = kai_int((int64_t) fd);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { fd_kv, port_kv };
    static const char *names[2] = { "fd", "port" };
    return kai_record(2, fields, names);
}

/* Read a named slot of a handle record. A missing or mistyped slot
 * reads as NULL / -1 (the caller falls through to an error return);
 * the typer keeps the shape honest. */
static KaiValue *_kai_net_record_slot(KaiValue *v, const char *name) {
    if (!v || v->tag != KAI_RECORD) return NULL;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], name) == 0) {
            return v->as.rec.fields[i];
        }
    }
    return NULL;
}

static int64_t _kai_net_record_int(KaiValue *v, const char *name) {
    KaiValue *f = _kai_net_record_slot(v, name);
    return (f && kai_is_int(f)) ? kai_intf(f) : -1;
}

static int _kai_net_record_fd(KaiValue *v) {
    return (int) _kai_net_record_int(v, "fd");
}

/* connect(host, port) -> Result[Conn, String]. host is a hostname
 * or IPv4 dotted-quad; getaddrinfo handles both. Restricted to
 * AF_INET in v1.
 *
 * Issue #630 — Phase R2: non-blocking connect. The fd is flipped to
 * O_NONBLOCK before connect(), which returns either 0 (instant
 * success — common for loopback) or -1 with errno == EINPROGRESS.
 * On EINPROGRESS the fiber parks on write-readiness; when the
 * handshake completes (or fails) the kernel marks the fd writable.
 * We read SO_ERROR to distinguish success from failure since
 * connect() itself does not run a second time. EAGAIN/EWOULDBLOCK
 * is not a documented connect() outcome and is treated identically
 * to EINPROGRESS for safety. */
KAI_SCHED_FN KaiValue *kai_default_nettcp_connect(void *self, KaiValue *host, KaiValue *port, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "connect: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%lld", (long long) kai_intf(port));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_buf, port_buf, &hints, &res);
    if (gai != 0) {
        const char *msg = gai_strerror(gai);
        return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
    }
    kai_reactor_init();
    int fd = -1;
    int saved_errno = 0;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { saved_errno = errno; continue; }
        kai_socket_set_nonblock(fd);
        int crc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (crc == 0) {
            saved_errno = 0;
            break;  /* instant success — loopback path */
        }
        if (errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Park until the kernel marks the fd writable. POLLHUP /
             * POLLERR also wake us — drain treats them as ready and
             * the SO_ERROR check below surfaces the real reason. */
            kai_reactor_park_socket_write(kai_current_fiber(), fd);
            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) < 0) {
                saved_errno = errno;
                close(fd);
                fd = -1;
                continue;
            }
            if (so_err == 0) {
                saved_errno = 0;
                break;  /* handshake succeeded */
            }
            saved_errno = so_err;
            close(fd);
            fd = -1;
            continue;
        }
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return _kai_net_err(k, saved_errno ? saved_errno : ECONNREFUSED);
    }
    KaiValue *conn = _kai_net_make_conn(fd);
    KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = conn}});
    return kai_cont_resume(k, ok);
}
#endif

/* listen(host, port) -> Result[Listener, String]. host = "" or
 * "0.0.0.0" binds INADDR_ANY; specific IPv4 string also works.
 * port = 0 asks the kernel for an ephemeral port; we read it back
 * via getsockname so callers don't need a separate effect op. */
static KaiValue *kai_default_nettcp_listen(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "listen: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);

    /* SO_REUSEADDR mirrors the convention every server example sets
     * — without it a listener that crashed seconds ago can't rebind
     * because the kernel still holds the port in TIME_WAIT. The
     * fixture's `bind(0)` path ignores the assigned port across
     * runs anyway, but explicit servers benefit. */
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) kai_intf(port));
    if (host_buf[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host_buf, &addr.sin_addr) != 1) {
        /* Fall back to getaddrinfo for hostnames. */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host_buf, NULL, &hints, &res);
        if (gai != 0) {
            close(fd);
            const char *msg = gai_strerror(gai);
            return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
        }
        struct sockaddr_in *sin = (struct sockaddr_in *) res->ai_addr;
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }
    if (listen(fd, KAI_LISTEN_BACKLOG) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }
    /* Issue #630 — Phase R2: the listener fd must be non-blocking so
     * the parking accept() path below sees EAGAIN/EWOULDBLOCK when
     * no connection is queued. Without this the accept() syscall
     * would block the OS thread and starve every other fiber. */
    kai_socket_set_nonblock(fd);

    /* Read back the assigned port (kernel may have picked one when
     * the caller passed 0). */
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    int actual_port = (int) kai_intf(port);
    if (getsockname(fd, (struct sockaddr *) &bound, &blen) == 0) {
        actual_port = (int) ntohs(bound.sin_port);
    }

    KaiValue *l  = _kai_net_make_listener(fd, actual_port);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = l}});
    return kai_cont_resume(k, ok);
}

/* accept(l) -> Result[Conn, String].
 *
 * Issue #630 — Phase R2: non-blocking accept. The listener fd was
 * flipped to O_NONBLOCK in listen(); accept() now returns -1 +
 * EAGAIN/EWOULDBLOCK when no peer is queued. The fiber parks on
 * read-readiness of the listener fd and retries on wake. The
 * connection fd inherits non-blocking on Linux when SOCK_NONBLOCK
 * is passed to accept4(); for portability (macOS lacks accept4)
 * we set it explicitly via kai_socket_set_nonblock on the returned
 * fd so subsequent send/recv on the Conn also park rather than
 * blocking. */
KAI_SCHED_FN KaiValue *kai_default_nettcp_accept(void *self, KaiValue *l, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int lfd = _kai_net_record_fd(l);
    if (lfd < 0) return _kai_net_err_msg(k, "accept: invalid listener");
    kai_reactor_init();
    /* Defensive: listener fd should already be non-blocking from
     * listen(), but a future Listener constructed via FFI would not
     * be. Idempotent. */
    kai_socket_set_nonblock(lfd);
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *) &peer, &plen);
        if (cfd >= 0) {
            kai_socket_set_nonblock(cfd);
            KaiValue *conn = _kai_net_make_conn(cfd);
            KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = conn}});
            return kai_cont_resume(k, ok);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kai_reactor_park_socket_read(kai_current_fiber(), lfd);
            /* On resume, kai_sched_park runs the cancel-yield check
             * for us (issue #679 fix) — a sibling-triggered cancel
             * longjmps to cancel_pad before the retry below. */
            continue;  /* Resume → retry accept() */
        }
        if (errno == EINTR) continue;
        return _kai_net_err(k, errno);
    }
}
#endif

/* send(c, data) -> Result[Int, String]. data is a [Byte] = [Int]
 * cons list; each element is taken mod 256. v1 walks the list once
 * to assemble a contiguous buffer, then writes it.
 *
 * Issue #630 — Phase R2: non-blocking send with a partial-writes
 * loop. The conn fd is already O_NONBLOCK (set by accept or
 * connect). send() may return fewer bytes than requested when the
 * kernel buffer is partially full, or -1 with EAGAIN when it is
 * completely full. The loop parks on write-readiness on EAGAIN and
 * advances on partial writes. The returned count is the total
 * bytes written — equal to the input length on success. The
 * pre-R2 contract that "callers may have to loop" is honoured
 * internally so user code never sees a short write. */
KAI_SCHED_FN KaiValue *kai_default_nettcp_send(void *self, KaiValue *c, KaiValue *data, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "send: invalid conn");
    if (!data) return _kai_net_err_msg(k, "send: null data");

    /* Count the cons cells; a [Byte] of millions of bytes is out of
     * scope for v1 — chunked send via streaming is a follow-up. */
    size_t n = 0;
    for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *) malloc(n);
        if (!buf) return _kai_net_err_msg(k, "send: out of memory");
        size_t i = 0;
        for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
            KaiValue *h = p->as.cons.head;
            int64_t b = (kai_is_int(h)) ? kai_intf(h) : 0;
            buf[i++] = (unsigned char) (b & 0xff);
        }
    }

    kai_reactor_init();
    /* Defensive: send may be called on a Conn that was never funnelled
     * through the local accept/connect path (e.g. constructed via FFI
     * in a future user lane). Idempotent on already-nonblock fds. */
    kai_socket_set_nonblock(fd);

    size_t total = 0;
    int saved_errno = 0;
    while (total < n) {
        /* MSG_NOSIGNAL stops a SIGPIPE from killing the process when
         * the peer has closed its read side; we surface EPIPE through
         * the Result-shaped return instead. macOS does not have
         * MSG_NOSIGNAL but exposes SO_NOSIGPIPE on the socket; we
         * conservatively pass the flag where it exists and rely on
         * the default SIGPIPE handler being SIG_IGN-equivalent on
         * the install side for v1. */
#ifdef MSG_NOSIGNAL
        ssize_t w = send(fd, buf + total, n - total, MSG_NOSIGNAL);
#else
        ssize_t w = send(fd, buf + total, n - total, 0);
#endif
        if (w > 0) {
            total += (size_t) w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            kai_reactor_park_socket_write(kai_current_fiber(), fd);
            continue;  /* Resume → retry send() */
        }
        if (w < 0 && errno == EINTR) continue;
        saved_errno = errno;
        break;
    }
    free(buf);
    if (saved_errno != 0 && total == 0) {
        return _kai_net_err(k, saved_errno);
    }

    KaiValue *cnt = kai_int((int64_t) total);
    KaiValue *ok  = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = cnt}});
    return kai_cont_resume(k, ok);
}
#endif

/* recv(c, max) -> Result[[Byte], String]. max = 0 panics per spec
 * (no useful "read zero bytes"); negative max is treated the same.
 * Peer clean-close (recv == 0) returns Ok([]) so callers can
 * distinguish from "not yet" via the empty list.
 *
 * Issue #630 — Phase R2: non-blocking recv. The fd is O_NONBLOCK
 * (set by accept or connect). recv() returns >0 (bytes available),
 * 0 (peer clean-close — surfaces as Ok([])), or -1 with EAGAIN
 * when the kernel buffer is empty. On EAGAIN the fiber parks on
 * read-readiness and retries. A single recv call returns whatever
 * the kernel has buffered; callers loop at the user level if they
 * need an exact byte count (matches POSIX recv semantics). */
KAI_SCHED_FN KaiValue *kai_default_nettcp_recv(void *self, KaiValue *c, KaiValue *max, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "recv: invalid conn");
    int64_t cap = (kai_is_int(max)) ? kai_intf(max) : 0;
    if (cap <= 0) {
        kai_eputs("kai: NetTcp.recv: max must be > 0\n");
        kai_exit(1);
    }
    if (cap > (1 << 20)) cap = 1 << 20;  /* 1 MiB ceiling for v1 */
    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv: out of memory");

    kai_reactor_init();
    /* Defensive idempotent flip — Conn fds from accept/connect are
     * already non-blocking, but a future FFI-constructed Conn would
     * not be. */
    kai_socket_set_nonblock(fd);

    ssize_t got;
    for (;;) {
        got = recv(fd, buf, (size_t) cap, 0);
        if (got >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            kai_reactor_park_socket_read(kai_current_fiber(), fd);
            continue;  /* Resume → retry recv() */
        }
        if (errno == EINTR) continue;
        int saved_errno = errno;
        free(buf);
        return _kai_net_err(k, saved_errno);
    }
    KaiValue *acc = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = acc}});
    return kai_cont_resume(k, ok);
}
#endif

/* recv_timeout(c, max, nanos) -> Option[Result[[Int], String]]. The
 * socket-side dual of Actor.receive_timeout: park on read-readiness and
 * a `nanos` deadline, resume on whichever fires first (reactor single-
 * list dual-park). `None` = deadline elapsed before any byte; `Some(Ok
 * (bytes))` = data (or `Ok([])` on a clean EOF); `Some(Err(msg))` = a
 * transport error. Three-way distinguishable, as the DoS fix requires. */
KAI_SCHED_FN KaiValue *kai_default_nettcp_recv_timeout(void *self, KaiValue *c,
                                                 KaiValue *max, KaiValue *ns,
                                                 KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "recv_timeout: invalid conn");
    int64_t cap = (kai_is_int(max)) ? kai_intf(max) : 0;
    if (cap <= 0) {
        kai_eputs("kai: NetTcp.recv_timeout: max must be > 0\n");
        kai_exit(1);
    }
    if (cap > (1 << 20)) cap = 1 << 20;  /* 1 MiB ceiling for v1 */
    int64_t budget = (kai_is_int(ns)) ? kai_intf(ns) : 0;
    if (budget < 0) budget = 0;

    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv_timeout: out of memory");

    kai_reactor_init();
    kai_socket_set_nonblock(fd);

    uint64_t deadline = kai_reactor_now_ns() + (uint64_t) budget;
    ssize_t got;
    for (;;) {
        got = recv(fd, buf, (size_t) cap, 0);
        if (got >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!kai_reactor_park_socket_read_timeout(kai_current_fiber(),
                                                      fd, deadline)) {
                free(buf);
                return kai_cont_resume(k, kai_variant_u(1, "None", 0, 0, NULL));
            }
            continue;  /* Readiness → retry recv() */
        }
        if (errno == EINTR) continue;
        int saved_errno = errno;
        free(buf);
        const char *m = strerror(saved_errno);
        if (!m) m = "unknown error";
        KaiValue *err = kai_variant_u(3, "Err", 1, 0,
                                      (KaiVarSlot[]){{.ptr = kai_str(m)}});
        KaiValue *some = kai_variant_u(0, "Some", 1, 0,
                                       (KaiVarSlot[]){{.ptr = err}});
        return kai_cont_resume(k, some);
    }
    KaiValue *acc = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; acc = kai_cons(kai_int((int64_t) buf[i]), acc); }
    free(buf);
    KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = acc}});
    KaiValue *some = kai_variant_u(0, "Some", 1, 0, (KaiVarSlot[]){{.ptr = ok}});
    return kai_cont_resume(k, some);
}
#endif

/* close(c) -> Unit. Spec says errors from close(2) are logged and
 * swallowed — the user can do nothing useful with the value, and
 * shutdown() is the right tool for "did everything flush?" anyway.
 * Returns kai_unit(). */
static KaiValue *kai_default_nettcp_close(void *self, KaiValue *c, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd >= 0) {
        if (close(fd) < 0) {
            /* Stderr only — the surface op returns Unit. */
            kai_eprintf("kai: NetTcp.close: %s\n", strerror(errno));
        }
    }
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * NetUnix default handler — Unix-domain stream sockets.
 * =================================================================
 *
 * accept / send / recv / recv_timeout / close bridge to the NetTcp
 * handlers above: past listen/connect they only see an fd. Surface
 * records:
 *
 *   Conn      = { fd: Int }
 *   Listener  = { fd: Int, path: String, dev: Int, ino: Int }
 *
 * `dev`/`ino` identify the filesystem node listen created, so
 * close_listener never unlinks a socket another process put at the
 * same path since.
 */

/* Copy a kaikai String into `buf` as a C path; -1 when it does not
 * fit or carries a NUL byte (which would silently truncate it). */
static int _kai_unix_cstr(KaiValue *s, char *buf, size_t cap) {
    if (!s || s->tag != KAI_STR || s->as.s.len >= cap) return -1;
    if (memchr(s->as.s.bytes, '\0', s->as.s.len)) return -1;
    memcpy(buf, s->as.s.bytes, s->as.s.len);
    buf[s->as.s.len] = '\0';
    return 0;
}

static int _kai_unix_addr(struct sockaddr_un *sa, const char *path) {
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(sa->sun_path)) return -1;
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    memcpy(sa->sun_path, path, n + 1);
    return 0;
}

static int _kai_unix_same_node(const char *path, int64_t dev, int64_t ino) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode)
        && (int64_t) st.st_dev == dev && (int64_t) st.st_ino == ino;
}

/* A socket file whose listener is gone refuses connections. The
 * probe is non-blocking, so a live listener with a full backlog
 * (EAGAIN) counts as live. */
static int _kai_unix_is_stale(const char *path) {
    struct sockaddr_un sa;
    if (_kai_unix_addr(&sa, path) < 0) return 0;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    kai_socket_set_nonblock(fd);
    int rc = connect(fd, (struct sockaddr *) &sa, sizeof(sa));
    int e = errno;
    close(fd);
    return rc < 0 && e == ECONNREFUSED;
}

/* Bind + chmod + listen at `tmp`, inside a 0700 directory: the socket
 * is unreachable until it already carries `mode`. umask is not used —
 * it is process-wide and would race the other scheduler threads. */
static int _kai_unix_bind_private(const char *tmp, mode_t mode, int *out_fd, struct stat *st) {
    struct sockaddr_un sa;
    if (_kai_unix_addr(&sa, tmp) < 0) return ENAMETOOLONG;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return errno;
    if (bind(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0 || chmod(tmp, mode) < 0
        || listen(fd, KAI_LISTEN_BACKLOG) < 0 || lstat(tmp, st) < 0) {
        int e = errno;
        close(fd);
        return e;
    }
    *out_fd = fd;
    return 0;
}

/* link(2), not rename(2): an occupied path fails with EEXIST instead
 * of being stolen. Only a stale socket is replaced, and only if the
 * node is still the one that was probed. */
static int _kai_unix_publish(const char *tmp, const char *path) {
    if (link(tmp, path) == 0) return 0;
    if (errno != EEXIST) return errno;
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) return EEXIST;
        if (!_kai_unix_is_stale(path)) return EADDRINUSE;
        if (_kai_unix_same_node(path, (int64_t) st.st_dev, (int64_t) st.st_ino)) unlink(path);
    }
    return link(tmp, path) == 0 ? 0 : errno;
}

/* The private directory sits beside `path` so link(2) stays on one
 * filesystem. Its socket path is 12 bytes longer than `path`'s
 * directory prefix — the sun_path limit applies to it too. */
static int _kai_unix_listen_at(const char *path, mode_t mode, int *fd, struct stat *st) {
    const char *slash = strrchr(path, '/');
    int plen = slash ? (int) (slash - path) + 1 : 0;
    char dir[KAI_PATH_BUF], tmp[KAI_PATH_BUF];
    int n = snprintf(dir, sizeof(dir), "%.*s.kaiXXXXXX", plen, path);
    if (n < 0 || (size_t) n >= sizeof(dir)) return ENAMETOOLONG;
    if (!mkdtemp(dir)) return errno;
    int e = chmod(dir, 0700) < 0 ? errno : 0;
    n = snprintf(tmp, sizeof(tmp), "%s/s", dir);
    if (e == 0 && (n < 0 || (size_t) n >= sizeof(tmp))) e = ENAMETOOLONG;
    if (e == 0) e = _kai_unix_bind_private(tmp, mode, fd, st);
    if (e == 0) {
        e = _kai_unix_publish(tmp, path);
        if (e != 0) close(*fd);
    }
    unlink(tmp);
    rmdir(dir);
    return e;
}

static KaiValue *_kai_unix_make_listener(int fd, const char *path, const struct stat *st) {
    KaiValue *fields[4] = {
        kai_int((int64_t) fd), kai_str_dyn(path),
        kai_int((int64_t) st->st_dev), kai_int((int64_t) st->st_ino)
    };
    static const char *names[4] = { "fd", "path", "dev", "ino" };
    return kai_record(4, fields, names);
}

static KaiValue *_kai_unix_ok(KaiCont *k, KaiValue *v) {
    return kai_cont_resume(k, kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = v}}));
}

/* listen(path, mode) -> Result[Listener, String]. */
static KaiValue *kai_default_netunix_listen(void *self, KaiValue *path, KaiValue *mode, KaiCont *k) {
    (void) self;
    char p[KAI_PATH_BUF];
    struct sockaddr_un probe;
    if (_kai_unix_cstr(path, p, sizeof(p)) < 0 || !kai_is_int(mode)) {
        return _kai_net_err_msg(k, "listen: bad arguments");
    }
    int64_t m = kai_intf(mode);
    if (m < 0 || m > 0777) return _kai_net_err_msg(k, "listen: mode outside 0..0o777");
    if (_kai_unix_addr(&probe, p) < 0) return _kai_net_err_msg(k, "listen: path too long for a Unix socket");
    int fd = -1;
    struct stat st;
    int e = _kai_unix_listen_at(p, (mode_t) m, &fd, &st);
    if (e == ENAMETOOLONG) return _kai_net_err_msg(k, "listen: path too long for a Unix socket");
    if (e != 0) return _kai_net_err(k, e);
    kai_socket_set_nonblock(fd);
    return _kai_unix_ok(k, _kai_unix_make_listener(fd, p, &st));
}

/* connect(path) -> Result[Conn, String]. A full backlog answers
 * EAGAIN (Linux); readiness cannot signal "backlog drained" on an
 * unconnected socket, so the fiber sleeps briefly and retries. */
KAI_SCHED_FN KaiValue *kai_default_netunix_connect(void *self, KaiValue *path, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{
    (void) self;
    char p[KAI_PATH_BUF];
    struct sockaddr_un sa;
    if (_kai_unix_cstr(path, p, sizeof(p)) < 0) return _kai_net_err_msg(k, "connect: bad arguments");
    if (_kai_unix_addr(&sa, p) < 0) return _kai_net_err_msg(k, "connect: path too long for a Unix socket");
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);
    kai_reactor_init();
    kai_socket_set_nonblock(fd);
    while (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0 && errno != EISCONN) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY) {
            kai_reactor_park_timer(kai_current_fiber(), kai_reactor_now_ns() + 1000000);
            continue;
        }
        int e = errno;
        close(fd);
        return _kai_net_err(k, e);
    }
    return _kai_unix_ok(k, _kai_net_make_conn(fd));
}
#endif

/* The peer's effective uid as the kernel recorded it at connect /
 * listen time — nothing the peer sends can forge it. */
static int _kai_unix_peer_uid(int fd, int64_t *uid) {
#if defined(SO_PEERCRED)
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0) return errno;
    *uid = (int64_t) cred.uid;
    return 0;
#elif defined(LOCAL_PEERCRED)
    struct xucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &len) < 0) return errno;
    if (cred.cr_version != XUCRED_VERSION) return EPROTO;
    *uid = (int64_t) cred.cr_uid;
    return 0;
#else
    (void) fd; (void) uid;
    return ENOTSUP;
#endif
}

/* peer_uid(c) -> Result[Int, String]. */
static KaiValue *kai_default_netunix_peer_uid(void *self, KaiValue *c, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(c);
    if (fd < 0) return _kai_net_err_msg(k, "peer_uid: invalid conn");
    int64_t uid = -1;
    int e = _kai_unix_peer_uid(fd, &uid);
    if (e != 0) return _kai_net_err(k, e);
    return _kai_unix_ok(k, kai_int(uid));
}

/* close_listener(l) -> Unit. Unlinks the socket file only while it is
 * still the node listen created, then closes the fd. */
static KaiValue *kai_default_netunix_close_listener(void *self, KaiValue *l, KaiCont *k) {
    (void) self;
    char p[KAI_PATH_BUF];
    if (_kai_unix_cstr(_kai_net_record_slot(l, "path"), p, sizeof(p)) == 0
        && _kai_unix_same_node(p, _kai_net_record_int(l, "dev"), _kai_net_record_int(l, "ino"))) {
        unlink(p);
    }
    int fd = _kai_net_record_fd(l);
    if (fd >= 0 && close(fd) < 0) {
        kai_eprintf("kai: NetUnix.close_listener: %s\n", strerror(errno));
    }
    return kai_cont_resume(k, kai_unit());
}

/* =================================================================
 * NetDns effect — issue #352. getaddrinfo(3) exposed as a standalone
 * capability so a component can resolve names without earning the
 * right to open sockets (NetTcp). The syscall shape is identical to
 * the one embedded in `kai_default_nettcp_connect`; the duplication
 * is small and intentional (sharing a helper is the out-of-scope
 * follow-up the issue names). AF_INET / SOCK_STREAM hints match the
 * TCP path so the address set resolve returns is the same set connect
 * would have walked — IPv4-only in v1, same limitation as NetTcp.
 * ================================================================= */

/* Build an IpAddr record `{ addr }` from an addrinfo node. The field
 * name "addr" is the runtime contract with the kaikai-side builtin
 * `type IpAddr = { addr: String }` (stdlib/net/dns.kai);
 * kai_op_field reads it by strcmp, so the static cstring is enough. */
static KaiValue *_kai_net_make_ipaddr(struct addrinfo *p) {
    char ip_buf[INET_ADDRSTRLEN];
    struct sockaddr_in *sin = (struct sockaddr_in *) p->ai_addr;
    const char *txt = inet_ntop(AF_INET, &sin->sin_addr, ip_buf, sizeof(ip_buf));
    KaiValue *addr_kv = kai_str(txt ? txt : "");
    KaiValue *fields[1] = { addr_kv };
    static const char *names[1] = { "addr" };
    return kai_record(1, fields, names);
}

/* Build a `[IpAddr]` from the getaddrinfo result list, preserving the
 * order getaddrinfo returned (recursing tail-first keeps the cons
 * chain in source order without a reverse pass). Non-AF_INET nodes
 * are skipped — v1 is IPv4-only and the hints already filter, but
 * the guard keeps the inet_ntop above honest. */
static KaiValue *_kai_net_ipaddr_list(struct addrinfo *p) {
    if (!p) return kai_nil();
    KaiValue *tail = _kai_net_ipaddr_list(p->ai_next);
    if (p->ai_family != AF_INET || !p->ai_addr) return tail;
    return kai_cons(_kai_net_make_ipaddr(p), tail);
}

/* resolve(host) -> Result[[IpAddr], String] (Ok-first). host is a
 * hostname or IPv4 dotted-quad; getaddrinfo handles both. An empty
 * result list (no AF_INET addresses) still resolves to `Ok([])` —
 * the stdlib `resolve_first` turns that into its own Err so the
 * runtime stays a thin getaddrinfo shim. */
static KaiValue *kai_default_netdns_resolve(void *self, KaiValue *host, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR) {
        return _kai_net_err_msg(k, "resolve: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host_buf, NULL, &hints, &res);
    if (gai != 0) {
        const char *msg = gai_strerror(gai);
        return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
    }
    KaiValue *list = _kai_net_ipaddr_list(res);
    freeaddrinfo(res);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = list}});
    return kai_cont_resume(k, ok);
}

/* =================================================================
 * NetUdp default handler (issue #354)
 * =================================================================
 *
 * Mirror of the NetUdp block in stage0/runtime.h. Stage 2 keeps its
 * own copy of the runtime (Koka-style Perceus RC), so the datagram
 * UDP handlers live here verbatim alongside the NetTcp family.
 *
 * Spec: docs/effects-stdlib.md §`NetUdp`. Four ops, all blocking in
 * v1 (no reactor parking yet — the m8.x reactor lifts NetTcp and
 * NetUdp together). socket(AF_INET, SOCK_DGRAM, 0) + bind / sendto /
 * recvfrom / close. Surface records:
 *
 *   UdpSocket  = { fd: Int, port: Int }
 *   SocketAddr = { host: String, port: Int }
 *
 * `[Byte]` lands as `[Int]`; SocketAddr.host is a textual IPv4
 * dotted-quad (inet_pton on send, inet_ntop on recv). IPv4 only.
 */

static KaiValue *_kai_net_make_udpsocket(int fd, int port) {
    KaiValue *fd_kv   = kai_int((int64_t) fd);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { fd_kv, port_kv };
    static const char *names[2] = { "fd", "port" };
    return kai_record(2, fields, names);
}

static KaiValue *_kai_net_make_sockaddr(const char *host, int port) {
    KaiValue *host_kv = kai_str_dyn(host);
    KaiValue *port_kv = kai_int((int64_t) port);
    KaiValue *fields[2] = { host_kv, port_kv };
    static const char *names[2] = { "host", "port" };
    return kai_record(2, fields, names);
}

static int _kai_net_sockaddr_host(KaiValue *v, char *out, size_t cap) {
    if (!v || v->tag != KAI_RECORD || cap == 0) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "host") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!f || f->tag != KAI_STR) return -1;
            size_t n = f->as.s.len < cap - 1 ? f->as.s.len : cap - 1;
            memcpy(out, f->as.s.bytes, n);
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

static int _kai_net_record_port(KaiValue *v) {
    if (!v || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "port") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

static KaiValue *kai_default_netudp_bind(void *self, KaiValue *host, KaiValue *port, KaiCont *k) {
    (void) self;
    if (!host || host->tag != KAI_STR || !kai_is_int(port)) {
        return _kai_net_err_msg(k, "bind: bad arguments");
    }
    char host_buf[KAI_NET_HOST_BUF];
    size_t hlen = host->as.s.len < sizeof(host_buf) - 1 ? host->as.s.len : sizeof(host_buf) - 1;
    memcpy(host_buf, host->as.s.bytes, hlen);
    host_buf[hlen] = '\0';

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return _kai_net_err(k, errno);

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    int64_t port_i = kai_intf(port);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t) port_i);
    if (host_buf[0] == '\0') {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, host_buf, &addr.sin_addr) != 1) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host_buf, NULL, &hints, &res);
        if (gai != 0) {
            close(fd);
            const char *msg = gai_strerror(gai);
            return _kai_net_err_msg(k, msg ? msg : "getaddrinfo failed");
        }
        struct sockaddr_in *sin = (struct sockaddr_in *) res->ai_addr;
        addr.sin_addr = sin->sin_addr;
        freeaddrinfo(res);
    }

    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        int e = errno; close(fd); return _kai_net_err(k, e);
    }

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    int actual_port = (int) port_i;
    if (getsockname(fd, (struct sockaddr *) &bound, &blen) == 0) {
        actual_port = (int) ntohs(bound.sin_port);
    }

    KaiValue *s  = _kai_net_make_udpsocket(fd, actual_port);
    KaiValue *ok = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = s}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_send(void *self, KaiValue *sock, KaiValue *dst, KaiValue *data, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd < 0) return _kai_net_err_msg(k, "send: invalid socket");
    if (!data) return _kai_net_err_msg(k, "send: null data");

    char dst_host[KAI_NET_HOST_BUF];
    if (_kai_net_sockaddr_host(dst, dst_host, sizeof(dst_host)) != 0) {
        return _kai_net_err_msg(k, "send: invalid destination address");
    }
    int dst_port = _kai_net_record_port(dst);
    if (dst_port < 0) return _kai_net_err_msg(k, "send: invalid destination port");

    struct sockaddr_in dst_addr;
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.sin_family = AF_INET;
    dst_addr.sin_port   = htons((uint16_t) dst_port);
    if (inet_pton(AF_INET, dst_host, &dst_addr.sin_addr) != 1) {
        return _kai_net_err_msg(k, "send: destination host is not an IPv4 address");
    }

    size_t n = 0;
    for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) ++n;
    unsigned char *buf = NULL;
    if (n > 0) {
        buf = (unsigned char *) malloc(n);
        if (!buf) return _kai_net_err_msg(k, "send: out of memory");
        size_t i = 0;
        for (KaiValue *p = data; p && p->tag == KAI_CONS; p = p->as.cons.tail) {
            KaiValue *h = p->as.cons.head;
            int64_t b = (kai_is_int(h)) ? kai_intf(h) : 0;
            buf[i++] = (unsigned char) (b & 0xff);
        }
    }

    ssize_t w;
    for (;;) {
#ifdef MSG_NOSIGNAL
        w = sendto(fd, buf, n, MSG_NOSIGNAL, (struct sockaddr *) &dst_addr, sizeof(dst_addr));
#else
        w = sendto(fd, buf, n, 0, (struct sockaddr *) &dst_addr, sizeof(dst_addr));
#endif
        if (w < 0 && errno == EINTR) continue;
        break;
    }
    int saved_errno = (w < 0) ? errno : 0;
    free(buf);
    if (w < 0) return _kai_net_err(k, saved_errno);

    KaiValue *cnt = kai_int((int64_t) w);
    KaiValue *ok  = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = cnt}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_recv(void *self, KaiValue *sock, KaiValue *max, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd < 0) return _kai_net_err_msg(k, "recv: invalid socket");
    int64_t cap = (kai_is_int(max)) ? kai_intf(max) : 0;
    if (cap <= 0) {
        kai_eputs("kai: NetUdp.recv: max must be > 0\n");
        kai_exit(1);
    }
    if (cap > (1 << 16)) cap = 1 << 16;  /* IPv4 datagram ceiling */
    unsigned char *buf = (unsigned char *) malloc((size_t) cap);
    if (!buf) return _kai_net_err_msg(k, "recv: out of memory");

    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    ssize_t got;
    for (;;) {
        memset(&src, 0, sizeof(src));
        srclen = sizeof(src);
        got = recvfrom(fd, buf, (size_t) cap, 0, (struct sockaddr *) &src, &srclen);
        if (got >= 0) break;
        if (errno == EINTR) continue;
        int saved_errno = errno;
        free(buf);
        return _kai_net_err(k, saved_errno);
    }

    char src_host[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &src.sin_addr, src_host, sizeof(src_host))) {
        src_host[0] = '\0';
    }
    int src_port = (int) ntohs(src.sin_port);
    KaiValue *addr = _kai_net_make_sockaddr(src_host, src_port);

    KaiValue *bytes = kai_nil();
    for (ssize_t i = got; i > 0;) { --i; bytes = kai_cons(kai_int((int64_t) buf[i]), bytes); }
    free(buf);

    KaiValue *pfields[2] = { addr, bytes };
    static const char *pnames[2] = { "fst", "snd" };
    KaiValue *pair = kai_record(2, pfields, pnames);
    KaiValue *ok   = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = pair}});
    return kai_cont_resume(k, ok);
}

static KaiValue *kai_default_netudp_close(void *self, KaiValue *sock, KaiCont *k) {
    (void) self;
    int fd = _kai_net_record_fd(sock);
    if (fd >= 0) {
        if (close(fd) < 0) {
            kai_eprintf("kai: NetUdp.close: %s\n", strerror(errno));
        }
    }
    return kai_cont_resume(k, kai_unit());
}
#else
KAI_SCHED_FN KaiValue *kai_default_nettcp_connect(void *self, KaiValue *host, KaiValue *port, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
static KaiValue *kai_default_nettcp_listen(void *self, KaiValue *host, KaiValue *port, KaiCont *k) { kai_host_absent("sockets"); }
KAI_SCHED_FN KaiValue *kai_default_nettcp_accept(void *self, KaiValue *l, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
KAI_SCHED_FN KaiValue *kai_default_nettcp_send(void *self, KaiValue *c, KaiValue *data, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
KAI_SCHED_FN KaiValue *kai_default_nettcp_recv(void *self, KaiValue *c, KaiValue *max, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
KAI_SCHED_FN KaiValue *kai_default_nettcp_recv_timeout(void *self, KaiValue *c, KaiValue *max, KaiValue *ns, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
static KaiValue *kai_default_nettcp_close(void *self, KaiValue *c, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netunix_listen(void *self, KaiValue *path, KaiValue *mode, KaiCont *k) { kai_host_absent("sockets"); }
KAI_SCHED_FN KaiValue *kai_default_netunix_connect(void *self, KaiValue *path, KaiCont *k)
#if KAI_SCHED_DECL_ONLY
;
#else
{ kai_host_absent("sockets"); }
#endif
static KaiValue *kai_default_netunix_peer_uid(void *self, KaiValue *c, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netunix_close_listener(void *self, KaiValue *l, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netdns_resolve(void *self, KaiValue *host, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netudp_bind(void *self, KaiValue *host, KaiValue *port, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netudp_send(void *self, KaiValue *sock, KaiValue *dst, KaiValue *data, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netudp_recv(void *self, KaiValue *sock, KaiValue *max, KaiCont *k) { kai_host_absent("sockets"); }
static KaiValue *kai_default_netudp_close(void *self, KaiValue *sock, KaiCont *k) { kai_host_absent("sockets"); }
#endif


