/* Fragment of runtime.h; include runtime.h, never this file. */
/* ---------- core: chunked / streaming file io (issue #771) ----------
 *
 * Five primitives behind the new `File` ops `open_read` / `read_chunk` /
 * `open_write` / `write_chunk` / `close_file`. Unlike the bulk
 * `read_file` / `write_file` pair these keep an OS file descriptor open
 * across calls, so the carrier (line surface, #801) can pull a large
 * file chunk-by-chunk without materialising it. The handle is an opaque
 * `FileHandle` record `{ fd: Int }` — same shape as the net `Conn`.
 *
 * Error register is `Result[_, String]` (Ok first), mirroring
 * `read_file`. `read_chunk` returns `Ok("")` at EOF (the spec'd
 * sentinel), never an error. Each primitive consumes its KaiValue *
 * args linearly, decref'ing before building the result.
 *
 * Stage2 boxes small Ints tagged, so the `fd` slot inside a FileHandle
 * record is read with kai_is_int / kai_intf — NEVER `->tag` / `->as.i`,
 * which segfault on a tagged 0x1. String args stay `->tag == KAI_STR`:
 * a String is always a heap pointer, never a tagged Int. */

/* Build a FileHandle record `{ fd }`. Field-name string matches the
 * kaikai-side `type FileHandle = { fd: Int }` decl
 * (builtin_filehandle_decl) — kai_op_field reads by strcmp. */
static KaiValue *_kai_file_make_handle(int fd) {
    KaiValue *fd_kv = kai_int((int64_t) fd);
    KaiValue *fields[1] = { fd_kv };
    static const char *names[1] = { "fd" };
    return kai_record(1, fields, names);
}

/* Pull the `fd` slot out of a FileHandle record. Returns -1 when the
 * value is the wrong shape (caller returns an Err). The slot is an Int,
 * which stage2 boxes tagged — read it with kai_is_int / kai_intf. */
static int _kai_file_handle_fd(KaiValue *v) {
    if (!kai_is_ptr(v) || v->tag != KAI_RECORD) return -1;
    for (int i = 0; i < v->as.rec.n_fields; ++i) {
        if (v->as.rec.names[i] && strcmp(v->as.rec.names[i], "fd") == 0) {
            KaiValue *f = v->as.rec.fields[i];
            if (!kai_is_int(f)) return -1;
            return (int) kai_intf(f);
        }
    }
    return -1;
}

static KaiValue *_kai_file_err(const char *msg) {
    KaiValue *m = kai_str(msg);
    return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = m}});
}

static KaiValue *_kai_file_ok(KaiValue *payload) {
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = payload}});
}

/* open_read(path) -> Result[FileHandle, String]. Opens `path` read-only
 * and hands back an owning FileHandle. */
static KaiValue *kai_core_file_open_read(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        r = _kai_file_err("open_read: argument is not a String");
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        int fd = open(pbuf, O_RDONLY);
        if (fd < 0) r = _kai_file_err("open_read: cannot open file");
        else        r = _kai_file_ok(_kai_file_make_handle(fd));
    }
    if (path) kai_decref(path);
    return r;
}

/* read_chunk(h, max) -> Result[String, String]. Reads up to `max` bytes
 * from the handle's fd into a fresh String. `Ok("")` signals EOF. A
 * short read (fewer than `max` bytes, but more than zero) is normal and
 * returned as-is; the caller loops until it sees the empty string. */
static KaiValue *kai_core_file_read_chunk(KaiValue *h, KaiValue *max) {
    KaiValue *r = NULL;
    int fd = _kai_file_handle_fd(h);
    int64_t cap = kai_is_int(max) ? kai_intf(max) : -1;
    if (fd < 0) {
        r = _kai_file_err("read_chunk: bad handle");
    } else if (cap < 0) {
        r = _kai_file_err("read_chunk: max is not a non-negative Int");
    } else if (cap == 0) {
        r = _kai_file_ok(kai_str(""));
    } else {
        char *buf = (char *) malloc((size_t) cap);
        if (!buf) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        ssize_t got;
        do { got = read(fd, buf, (size_t) cap); } while (got < 0 && errno == EINTR);
        if (got < 0) {
            free(buf);
            r = _kai_file_err("read_chunk: read failed");
        } else {
            KaiValue *v = kai_alloc(KAI_STR);
            v->as.s.len = (size_t) got;
            v->as.s.bytes = (char *) kai_heap_malloc((size_t) got + 1);
            memcpy(v->as.s.bytes, buf, (size_t) got);
            v->as.s.bytes[got] = '\0';
            free(buf);
            r = _kai_file_ok(v);
        }
    }
    if (kai_is_ptr(h))   kai_decref(h);
    if (kai_is_ptr(max)) kai_decref(max);
    return r;
}

/* open_write(path) -> Result[FileHandle, String]. Opens `path` for
 * writing, creating it (mode 0644) and truncating any existing
 * contents, then hands back an owning FileHandle. */
static KaiValue *kai_core_file_open_write(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        r = _kai_file_err("open_write: argument is not a String");
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        /* O_RDWR, not O_WRONLY: the handle's typed capability is
         * read + write, so reads through it must be valid. */
        int fd = open(pbuf, O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) r = _kai_file_err("open_write: cannot open file");
        else        r = _kai_file_ok(_kai_file_make_handle(fd));
    }
    if (path) kai_decref(path);
    return r;
}

/* write_chunk(h, data) -> Result[Unit, String]. Writes every byte of
 * `data` to the handle's fd, looping over partial writes. */
static KaiValue *kai_core_file_write_chunk(KaiValue *h, KaiValue *data) {
    KaiValue *r = NULL;
    int fd = _kai_file_handle_fd(h);
    if (fd < 0) {
        r = _kai_file_err("write_chunk: bad handle");
    } else if (!data || data->tag != KAI_STR) {
        r = _kai_file_err("write_chunk: data is not a String");
    } else {
        size_t total = data->as.s.len;
        size_t off = 0;
        int failed = 0;
        while (off < total) {
            ssize_t w = write(fd, data->as.s.bytes + off, total - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                failed = 1;
                break;
            }
            off += (size_t) w;
        }
        if (failed) r = _kai_file_err("write_chunk: write failed");
        else        r = _kai_file_ok(kai_unit());
    }
    if (kai_is_ptr(h))    kai_decref(h);
    if (kai_is_ptr(data)) kai_decref(data);
    return r;
}

/* close_file(h) -> Unit. Closes the underlying fd; a bad handle or a
 * close error is swallowed (there is no Result register on close — the
 * fd is gone either way). */
static KaiValue *kai_core_file_close(KaiValue *h) {
    int fd = _kai_file_handle_fd(h);
    if (fd >= 0) close(fd);
    if (kai_is_ptr(h)) kai_decref(h);
    return kai_unit();
}

/* Issue #345: file_exists / file_delete / file_rename. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * matching the core convention used by read_file/write_file above. */

static KaiValue *kai_core_file_exists(KaiValue *path) {
    int present = 0;
    if (kai_is_ptr(path) && path->tag == KAI_STR) {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        present = (access(pbuf, F_OK) == 0) ? 1 : 0;
    }
    if (path) kai_decref(path);
    return kai_bool(present);
}

static KaiValue *kai_core_file_delete(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_delete: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (unlink(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("file_delete: unlink failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_core_file_rename(KaiValue *from, KaiValue *to) {
    KaiValue *r = NULL;
    if (!from || from->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: from is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!to || to->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_rename: to is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char fbuf[KAI_PATH_BUF];
        char tbuf[KAI_PATH_BUF];
        size_t flen = from->as.s.len < sizeof(fbuf) - 1 ? from->as.s.len : sizeof(fbuf) - 1;
        size_t tlen = to->as.s.len   < sizeof(tbuf) - 1 ? to->as.s.len   : sizeof(tbuf) - 1;
        memcpy(fbuf, from->as.s.bytes, flen); fbuf[flen] = '\0';
        memcpy(tbuf, to->as.s.bytes,   tlen); tbuf[tlen] = '\0';
        if (rename(fbuf, tbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("file_rename: rename failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (from) kai_decref(from);
    if (to)   kai_decref(to);
    return r;
}

/* Issue #482 (follow-up to #345) + Array[Byte] refactor (prereq for
 * #452): binary file IO. Operates on `Array[Byte]` so the buffer
 * lines up with BinSerialize post-#488 (O(1) reads, contiguous
 * storage). Both primitives consume their args linearly (kai_decref
 * before allocating the result), matching the convention used by the
 * text variants. */

static KaiValue *kai_core_file_read_bytes(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_read_bytes: argument is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "rb");
        struct stat st;
        if (!fp) {
            KaiValue *msg = kai_str("file_read_bytes: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) {
            /* fopen accepts directories; on Linux ext4 ftell(SEEK_END) then
             * reports the htree EOF sentinel (~2^63), so reject before
             * sizing the buffer. */
            fclose(fp);
            KaiValue *msg = kai_str("file_read_bytes: is a directory");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else if (fseek(fp, 0, SEEK_END) != 0) {
            fclose(fp);
            KaiValue *msg = kai_str("file_read_bytes: seek failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            long n = ftell(fp);
            if (n < 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: tell failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else if (fseek(fp, 0, SEEK_SET) != 0) {
                fclose(fp);
                KaiValue *msg = kai_str("file_read_bytes: rewind failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                /* Allocating the kai_array directly (instead of
                 * kai_array_make + array_set in a loop) avoids the
                 * redundant default-incref/decref pair on every
                 * position. */
                unsigned char *buf = (unsigned char *) malloc((size_t) n + 1);
                if (!buf) { fclose(fp); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                size_t got = fread(buf, 1, (size_t) n, fp);
                fclose(fp);
                KaiValue *arr = kai_alloc(KAI_ARRAY);
                arr->as.arr.len = (int64_t) got;
                arr->as.arr.cap = got > 0 ? (int64_t) got : 1;
                arr->as.arr.items = (KaiValue **) kai_heap_malloc((size_t) arr->as.arr.cap * sizeof(KaiValue *));
                for (size_t i = 0; i < got; ++i) arr->as.arr.items[i] = kai_byte(buf[i]);
                free(buf);
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = arr}});
            }
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_core_file_write_bytes(KaiValue *path, KaiValue *bytes) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("file_write_bytes: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else if (!bytes || bytes->tag != KAI_ARRAY) {
        KaiValue *msg = kai_str("file_write_bytes: buffer is not an Array");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        FILE *fp = fopen(pbuf, "wb");
        if (!fp) {
            KaiValue *msg = kai_str("file_write_bytes: cannot open file");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        } else {
            /* Pack the kai_array into a contiguous C buffer, then
             * issue a single fwrite. Cheaper than per-slot fwrite
             * even with stdio buffering, and lets us fail fast on
             * a non-Byte slot before any IO. */
            int64_t n = bytes->as.arr.len;
            int ok = 1;
            unsigned char *out = NULL;
            if (n > 0) {
                out = (unsigned char *) malloc((size_t) n);
                if (!out) { fclose(fp); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                for (int64_t i = 0; i < n; ++i) {
                    KaiValue *e = bytes->as.arr.items[i];
                    if (!e || e->tag != KAI_BYTE) { ok = 0; break; }
                    out[i] = e->as.byte_val;
                }
            }
            if (ok && n > 0) {
                if (fwrite(out, 1, (size_t) n, fp) != (size_t) n) ok = 0;
            }
            if (out) free(out);
            fclose(fp);
            if (!ok) {
                KaiValue *msg = kai_str("file_write_bytes: write failed");
                r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
            } else {
                KaiValue *u = kai_unit();
                r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
            }
        }
    }
    if (path)  kai_decref(path);
    if (bytes) kai_decref(bytes);
    return r;
}

/* Issue #344: directory ops on top of the `File` effect. Each consumes
 * its String args linearly (kai_decref before allocating the result),
 * mirroring the file_exists/_delete/_rename convention. POSIX only
 * (macOS + Linux). Return shapes:
 *
 *   dir_list_dir(path)    : [String]              — entries (no . / ..)
 *   dir_create_dir(path)  : Result[Unit, String]  — Ok first
 *   dir_remove_dir(path)  : Result[Unit, String]
 *   dir_walk(path)        : [String]              — files (not dirs),
 *                                                   depth-first; symlinks
 *                                                   are NOT followed in v1
 *
 * dir_list_dir returns the empty list on read errors (matching how
 * args() can be empty); use dir_create_dir / dir_remove_dir when an
 * explicit Result is wanted. */

static KaiValue *kai_core_dir_list_dir(KaiValue *path) {
    KaiValue *acc = kai_nil();
    if (kai_is_ptr(path) && path->tag == KAI_STR) {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        DIR *d = opendir(pbuf);
        if (d) {
            /* Buffer entries then prepend in reverse so the caller sees
             * them in readdir order. readdir order is filesystem-defined
             * but stable for a given mount, which keeps test output
             * deterministic enough for fixtures. */
            size_t cap = 16, n = 0;
            char **names = (char **) malloc(cap * sizeof(char *));
            if (!names) { closedir(d); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *nm = e->d_name;
                if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
                if (n + 1 > cap) {
                    cap *= 2;
                    names = (char **) realloc(names, cap * sizeof(char *));
                    if (!names) { closedir(d); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                }
                size_t len = strlen(nm);
                char *copy = (char *) malloc(len + 1);
                if (!copy) { closedir(d); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                memcpy(copy, nm, len + 1);
                names[n++] = copy;
            }
            closedir(d);
            for (size_t i = n; i > 0;) {
                --i;
                acc = kai_cons(kai_str_dyn(names[i]), acc);
                free(names[i]);
            }
            free(names);
        }
    }
    if (path) kai_decref(path);
    return acc;
}

static KaiValue *kai_core_dir_create_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_create_dir: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (mkdir(pbuf, KAI_DIR_MODE) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("dir_create_dir: mkdir failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

static KaiValue *kai_core_dir_remove_dir(KaiValue *path) {
    KaiValue *r = NULL;
    if (!path || path->tag != KAI_STR) {
        KaiValue *msg = kai_str("dir_remove_dir: path is not a String");
        r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    } else {
        char pbuf[KAI_PATH_BUF];
        size_t plen = path->as.s.len < sizeof(pbuf) - 1 ? path->as.s.len : sizeof(pbuf) - 1;
        memcpy(pbuf, path->as.s.bytes, plen);
        pbuf[plen] = '\0';
        if (rmdir(pbuf) == 0) {
            KaiValue *u = kai_unit();
            r = kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = u}});
        } else {
            KaiValue *msg = kai_str("dir_remove_dir: rmdir failed");
            r = kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
        }
    }
    if (path) kai_decref(path);
    return r;
}

/* Iterative depth-first walk. Push subdirectories onto an explicit stack
 * so a deep tree doesn't blow the C stack. Symlinks are NOT followed
 * (v1 contract): use lstat + S_ISLNK skip so a symlink loop can't
 * trap the walker. Only regular files are emitted; directories are
 * traversed but not reported. */
static KaiValue *kai_core_dir_walk(KaiValue *root) {
    KaiValue *acc = kai_nil();
    if (!root || root->tag != KAI_STR) {
        if (root) kai_decref(root);
        return acc;
    }

    size_t cap = 16, top = 0;
    char **stack = (char **) malloc(cap * sizeof(char *));
    if (!stack) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }

    char rbuf[KAI_PATH_BUF];
    size_t rlen = root->as.s.len < sizeof(rbuf) - 1 ? root->as.s.len : sizeof(rbuf) - 1;
    memcpy(rbuf, root->as.s.bytes, rlen);
    rbuf[rlen] = '\0';
    {
        char *copy = (char *) malloc(rlen + 1);
        if (!copy) { free(stack); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        memcpy(copy, rbuf, rlen + 1);
        stack[top++] = copy;
    }

    /* Collect files first, then prepend in reverse so the cons list
     * reads in walk order. */
    size_t fcap = 32, fn = 0;
    char **files = (char **) malloc(fcap * sizeof(char *));
    if (!files) { free(stack[0]); free(stack); kai_eprintf("kai: out of memory\n"); kai_exit(1); }

    while (top > 0) {
        char *dir_path = stack[--top];
        DIR *d = opendir(dir_path);
        if (!d) { free(dir_path); continue; }
        /* Buffer child entries so we can push subdirs in reverse for
         * deterministic depth-first order. */
        size_t ccap = 16, cn = 0;
        char **child_paths = (char **) malloc(ccap * sizeof(char *));
        if (!child_paths) { closedir(d); free(dir_path); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        char *child_kinds = (char *) malloc(ccap); /* 'f' / 'd' / 's' (skip) */
        if (!child_kinds) { free(child_paths); closedir(d); free(dir_path); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0'))) continue;
            size_t dlen = strlen(dir_path);
            size_t nlen = strlen(nm);
            char *full = (char *) malloc(dlen + 1 + nlen + 1);
            if (!full) { closedir(d); free(dir_path); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
            memcpy(full, dir_path, dlen);
            int needs_sep = (dlen > 0 && dir_path[dlen - 1] != '/');
            size_t off = dlen;
            if (needs_sep) { full[off++] = '/'; }
            memcpy(full + off, nm, nlen + 1);
            struct stat st;
            char kind = 's';
            if (lstat(full, &st) == 0) {
                if (S_ISLNK(st.st_mode))      kind = 's'; /* skip symlinks */
                else if (S_ISDIR(st.st_mode)) kind = 'd';
                else if (S_ISREG(st.st_mode)) kind = 'f';
                else                          kind = 's'; /* sockets, fifos, etc. */
            }
            if (cn + 1 > ccap) {
                ccap *= 2;
                child_paths = (char **) realloc(child_paths, ccap * sizeof(char *));
                child_kinds = (char *)  realloc(child_kinds, ccap);
                if (!child_paths || !child_kinds) { closedir(d); free(dir_path); kai_eprintf("kai: out of memory\n"); kai_exit(1); }
            }
            child_paths[cn] = full;
            child_kinds[cn] = kind;
            cn++;
        }
        closedir(d);
        free(dir_path);
        /* Emit files in encountered order. */
        for (size_t i = 0; i < cn; ++i) {
            char k = child_kinds[i];
            if (k == 'f') {
                if (fn + 1 > fcap) {
                    fcap *= 2;
                    files = (char **) realloc(files, fcap * sizeof(char *));
                    if (!files) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                }
                files[fn++] = child_paths[i];
            }
        }
        /* Push subdirs in reverse onto the stack so they pop in natural order. */
        for (size_t i = cn; i > 0;) {
            --i;
            char k = child_kinds[i];
            if (k == 'd') {
                if (top + 1 > cap) {
                    cap *= 2;
                    stack = (char **) realloc(stack, cap * sizeof(char *));
                    if (!stack) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
                }
                stack[top++] = child_paths[i];
            } else if (k != 'f') {
                /* skipped — free the path string */
                free(child_paths[i]);
            }
        }
        free(child_paths);
        free(child_kinds);
    }
    free(stack);

    /* Build the cons list in reverse so consumers see files in walk order. */
    for (size_t i = fn; i > 0;) {
        --i;
        acc = kai_cons(kai_str(files[i]), acc);
        free(files[i]);
    }
    free(files);

    kai_decref(root);
    return acc;
}

/* Process-wide stdin buffer shared by the Stdin handler's read_line and
 * read_bytes. Bytes read ahead of a consumer stay here, so every
 * consumer drains it before touching the fd. */
#define KAI_STDIN_BUF_SIZE 65536
static char kai_stdin_buf[KAI_STDIN_BUF_SIZE];
static size_t kai_stdin_pos = 0, kai_stdin_len = 0;
static pthread_mutex_t kai_stdin_mu = PTHREAD_MUTEX_INITIALIZER;

/* Moves up to `want` buffered bytes into `dst`; returns the count. */
static size_t kai_stdin_drain(char *dst, size_t want) {
    pthread_mutex_lock(&kai_stdin_mu);
    size_t take = kai_stdin_len - kai_stdin_pos;
    if (take > want) take = want;
    memcpy(dst, kai_stdin_buf + kai_stdin_pos, take);
    kai_stdin_pos += take;
    pthread_mutex_unlock(&kai_stdin_mu);
    return take;
}

/* Appends one line fragment to `*acc`; returns 1 once the newline was
 * consumed (the fragment excludes it), 0 when the buffer ran dry
 * first, -1 when there is nothing buffered and the fd read returned
 * `*rc` (0 = EOF, <0 = errno set). */
static int kai_stdin_take_line(char **acc, size_t *an, size_t *acap, ssize_t *rc) {
    pthread_mutex_lock(&kai_stdin_mu);
    if (kai_stdin_pos == kai_stdin_len) {
        ssize_t r = read(STDIN_FILENO, kai_stdin_buf, KAI_STDIN_BUF_SIZE);
        if (r <= 0) {
            int saved = errno;
            pthread_mutex_unlock(&kai_stdin_mu);
            errno = saved;
            *rc = r;
            return -1;
        }
        kai_stdin_pos = 0;
        kai_stdin_len = (size_t) r;
    }
    const char *start = kai_stdin_buf + kai_stdin_pos;
    size_t avail = kai_stdin_len - kai_stdin_pos;
    const char *nl = (const char *) memchr(start, '\n', avail);
    size_t take = nl ? (size_t) (nl - start) : avail;
    if (*an + take + 1 > *acap) {
        size_t cap = *acap ? *acap : KAI_READ_BUF_INIT;
        while (*an + take + 1 > cap) cap *= 2;
        *acc = (char *) realloc(*acc, cap);
        if (!*acc) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
        *acap = cap;
    }
    memcpy(*acc + *an, start, take);
    *an += take;
    kai_stdin_pos += take + (nl ? 1 : 0);
    pthread_mutex_unlock(&kai_stdin_mu);
    return nl ? 1 : 0;
}

static KaiValue *kai_core_read_line(void) {
    kai_flush_stdout();
    char stack_buf[512];
    char *buf = stack_buf;
    size_t cap = sizeof stack_buf, n = 0;
    int ch;
    flockfile(stdin);
    while ((ch = getc_unlocked(stdin)) != EOF && ch != '\n') {
        if (n + 1 >= cap) {
            cap *= 2;
            char *grown = (char *) (buf == stack_buf ? malloc(cap) : realloc(buf, cap));
            if (!grown) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
            if (buf == stack_buf) memcpy(grown, stack_buf, n);
            buf = grown;
        }
        buf[n++] = (char) ch;
    }
    funlockfile(stdin);
    if (ch == EOF && n == 0) {
        KaiValue *msg = kai_str("read_line: end of input");
        return kai_variant_u(3, "Err", 1, 0, (KaiVarSlot[]){{.ptr = msg}});
    }
    KaiValue *s = kai_str_from_bytes(buf, n);
    if (buf != stack_buf) free(buf);
    return kai_variant_u(2, "Ok", 1, 0, (KaiVarSlot[]){{.ptr = s}});
}

/* Issue #453 + #620: byte-oriented stdin read. Reads up to `n` raw
 * bytes from stdin (including '\n') and returns them as a String.
 * On EOF the returned String is shorter than `n` — possibly empty.
 * Used by LSP-style framed protocols where the body length is known
 * up front and may contain newlines.
 *
 * R3 unification: this core entry and the `Stdin.read_bytes`
 * default handler both go through `read(STDIN_FILENO, …)` so that a
 * program mixing the two forms (flat `read_bytes(n)` and qualified
 * `Stdin.read_bytes(n)`) consumes the input stream byte-for-byte
 * without libc's stdio buffer in the middle. The flat core path
 * stays blocking (it predates the reactor and is reachable from
 * stage 0 binaries that have no fibers); the qualified handler
 * parks the fiber on EAGAIN. */
static KaiValue *kai_core_read_bytes(KaiValue *n) {
    int64_t want = 0;
    if (n && kai_is_int(n) && kai_intf(n) > 0) want = kai_intf(n);
    if (n) kai_decref(n);
    if (want <= 0) return kai_str_from_bytes("", 0);
    char *buf = (char *) malloc((size_t) want);
    if (!buf) { kai_eprintf("kai: out of memory\n"); kai_exit(1); }
    size_t got = kai_stdin_drain(buf, (size_t) want);
    while (got < (size_t) want) {
        ssize_t r = read(STDIN_FILENO, buf + got, (size_t) want - got);
        if (r > 0) {
            got += (size_t) r;
        } else if (r == 0) {
            break;  /* EOF */
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* fd 0 is O_NONBLOCK because the R3 reactor handler
             * flipped it earlier in this process. The flat core
             * has no fiber to park; emulate blocking semantics by
             * polling until the fd is readable, then retry. */
#if KAI_HOST_REACTOR
            struct pollfd p = { STDIN_FILENO, POLLIN, 0 };
            kai_flush_stdout();
            int prc = poll(&p, 1, -1);
            if (prc < 0 && errno != EINTR) break;
            continue;
#else
            kai_host_absent("reactor");
#endif
        } else {
            break;  /* I/O error — surface as short read */
        }
    }
    KaiValue *s = kai_str_from_bytes(buf, got);
    free(buf);
    return s;
}

