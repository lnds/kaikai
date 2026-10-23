// Host for a kaikai module built for wasm32: supplies the WASI imports the
// module uses and routes fd 1 and 2 to the console and the page.

const ERRNO_BADF = 8;
const ERRNO_NOSYS = 52;
const ERRNO_SPIPE = 70;

class ProcExit extends Error {
  constructor(code) {
    super(`exit ${code}`);
    this.code = code;
  }
}

function lineSink(emit, echo) {
  let pending = "";
  const decoder = new TextDecoder();
  return {
    write(bytes) {
      pending += decoder.decode(bytes, { stream: true });
      let nl;
      while ((nl = pending.indexOf("\n")) >= 0) {
        emit(pending.slice(0, nl));
        echo?.(pending.slice(0, nl + 1));
        pending = pending.slice(nl + 1);
      }
    },
    flush() {
      if (pending) {
        emit(pending);
        echo?.(pending);
        pending = "";
      }
    },
  };
}

const SEG = /[^.]+/g;

// The JavaScript values a program holds as handles; slot 0 is never used.
function handleTable() {
  const slots = new Map();
  const free = [];
  let next = 1;
  return {
    put(v) {
      const i = free.length ? free.pop() : next++;
      slots.set(i, v);
      return i;
    },
    get: (i) => slots.get(i),
    release(i) {
      if (slots.delete(i)) free.push(i);
    },
    live: () => slots.size,
  };
}

// An import's name is `<param letters>:<return letter>:<path>`; see emit_ffi_js.kai.
function parseImport(name) {
  const [params, ret, ...rest] = name.split(":");
  const path = rest.join(":");
  return { params, ret, path, member: path.startsWith("."), write: path.endsWith("=") };
}

function jsImports(module, ctx) {
  const out = {};
  for (const { module: m, name } of WebAssembly.Module.imports(module)) {
    if (m !== "kai_js") continue;
    if (name === "release") out[name] = (i) => ctx.handles.release(i);
    else if (name === "take") out[name] = (dst, n) => ctx.take(dst, n);
    else out[name] = importFunction(parseImport(name), ctx);
  }
  return out;
}

function rootOf(first, ctx) {
  if (ctx.imports && first in ctx.imports) return ctx.imports;
  return first in ctx.builtins ? ctx.builtins : globalThis;
}

function importFunction(sig, ctx) {
  const segs = sig.path.replace(/^\./, "").replace(/=$/, "").match(SEG);
  const last = segs[segs.length - 1];
  const bytes = last.startsWith("raw_");
  return (...raw) => {
    const args = [];
    let k = 0;
    for (const c of sig.params) {
      if (c === "S") {
        args.push(bytes ? ctx.bytes(raw[k], raw[k + 1]) : ctx.decode(raw[k], raw[k + 1]));
        k += 2;
      } else {
        const v = raw[k++];
        args.push(c === "H" ? ctx.handles.get(v) : c === "B" ? v !== 0 : v);
      }
    }
    let parent = sig.member ? args.shift() : rootOf(segs[0], ctx);
    for (const s of segs.slice(0, -1)) parent = parent[s];
    let result;
    if (sig.write) {
      parent[last] = args[0];
    } else if (typeof parent[last] === "function") {
      result = parent[last](...args);
    } else if (args.length === 0) {
      result = parent[last];
    } else {
      throw new TypeError(`${sig.path} is not a function`);
    }
    return ctx.result(sig.ret, result);
  };
}

async function load(wasm) {
  if (wasm instanceof WebAssembly.Module) return wasm;
  if (wasm instanceof Uint8Array || wasm instanceof ArrayBuffer) {
    return WebAssembly.compile(wasm);
  }
  if (typeof process !== "undefined" && !(wasm instanceof URL && wasm.protocol.startsWith("http"))) {
    const { readFile } = await import("node:fs/promises");
    return WebAssembly.compile(await readFile(wasm));
  }
  return WebAssembly.compileStreaming(fetch(wasm));
}

class ContextEnd extends Error {}

const NO_JSPI =
  "kai: this program spawns fibers or keeps continuations, which need JavaScript Promise Integration (JSPI); this engine has none";

class NoJspi extends Error {}

const refuse = () => { throw new NoJspi(NO_JSPI); };

// Contexts are WebAssembly stacks the host keeps suspended. The running one
// suspends in `ctx_switch`; the host then starts or wakes the target. One
// context runs at a time.
function suspension(getInstance, events) {
  const waiting = new Map();
  const entries = new Map();
  let fail;
  const failed = new Promise((_, reject) => { fail = reject; });
  const enter = (fn) => {
    if (!entries.has(fn)) {
      entries.set(fn, WebAssembly.promising(getInstance().exports.__indirect_function_table.get(fn)));
    }
    return entries.get(fn);
  };
  const dispatch = (to, sp, fn) => {
    const wake = waiting.get(to);
    if (wake) {
      waiting.delete(to);
      wake();
      return;
    }
    getInstance().exports.__stack_pointer.value = sp;
    enter(fn)(to).catch((e) => { if (!(e instanceof ContextEnd)) fail(e); });
  };
  return {
    failed,
    imports: {
      needs_suspension: () => {},
      wait_until: new WebAssembly.Suspending((deadlineMs) => events.wait(deadlineMs)),
      ctx_switch: new WebAssembly.Suspending((from, to, sp, fn) => {
        const parked = new Promise((resolve) => waiting.set(from, resolve));
        dispatch(to, sp, fn);
        return parked;
      }),
      ctx_exit: (to, sp, fn) => {
        queueMicrotask(() => dispatch(to, sp, fn));
        throw new ContextEnd();
      },
    },
  };
}

const FRAME_MS = 1000 / 60;
const FRAME_SLACK_MS = FRAME_MS / 2;
const FRAME_FALLBACK_MS = 50;

// Host events: the page posts values, the program drains them, and a post wakes a
// wait the program is parked in. A post made while no wait is pending is latched,
// so the next wait returns at once. A wake from the host resolves the wait with -1,
// which tells the runtime to unpark the fibers waiting for host events.
export function hostEvents() {
  const queue = [];
  let wake = null;
  let latched = false;
  const nudge = () => {
    if (wake) wake();
    else latched = true;
  };
  return {
    post(event) {
      queue.push(event);
      nudge();
    },
    poke: nudge,
    take: (n = queue.length) => queue.splice(0, n),
    wait(deadlineMs) {
      if (latched) {
        latched = false;
        return Promise.resolve(-1);
      }
      return new Promise((resolve) => {
        const timers = [];
        let frame = 0;
        const done = (slack) => {
          timers.forEach(clearTimeout);
          if (frame) cancelAnimationFrame(frame);
          wake = null;
          resolve(slack);
        };
        wake = () => done(-1);
        const delay = deadlineMs < 0 ? Infinity : Math.max(0, deadlineMs - performance.now());
        if (delay <= FRAME_MS && typeof requestAnimationFrame === "function") {
          // A deadline within one frame waits for the frame, so a 16 ms loop
          // lands on every vsync instead of drifting past it.
          const onFrame = (ts) => {
            if (ts >= deadlineMs - FRAME_SLACK_MS) done(Math.max(0, deadlineMs - ts));
            else frame = requestAnimationFrame(onFrame);
          };
          frame = requestAnimationFrame(onFrame);
          timers.push(setTimeout(() => done(0), delay + FRAME_FALLBACK_MS));
        } else if (delay !== Infinity) {
          timers.push(setTimeout(() => done(0), delay));
        }
      });
    },
  };
}

const PREVENTED_KEYS = /^(Arrow|Page|Home$|End$| $)/;

// What the page gives a program that waits for it: events delivered as values, fetches and
// frames settled in the background, and pixel buffers. Everything a program reads back
// goes through these calls; nothing in here calls into the program.
function pageBuiltins(events, handles) {
  let current = {};
  const fetches = [];
  let frame = -1;
  const num = (v) => Number(v);
  return {
    is_null: (v) => v == null,
    set_style: (node, name, value) => node.style.setProperty(name, value),
    storage_has: (key) => localStorage.getItem(key) !== null,
    listen(node, kind) {
      node.addEventListener(kind, (e) => {
        if (kind === "keydown" && PREVENTED_KEYS.test(e.key ?? "")) e.preventDefault();
        events.post({
          kind,
          key: e.key ?? "",
          x: Math.round(e.offsetX ?? e.clientX ?? 0),
          y: Math.round(e.offsetY ?? e.clientY ?? 0),
          target: e.target,
        });
      });
    },
    interval(ms, kind) {
      setInterval(() => events.post({ kind, key: "", x: 0, y: 0, target: null }), num(ms));
    },
    next_event() {
      const [e] = events.take(1);
      if (!e) return false;
      current = e;
      return true;
    },
    event_kind: () => current.kind,
    event_key: () => current.key,
    event_x: () => current.x,
    event_y: () => current.y,
    event_target: () => current.target,
    fetch_start(url) {
      const slot = { done: false, ok: false, status: 0, body: "" };
      fetches.push(slot);
      fetch(url)
        .then(async (r) => Object.assign(slot, { ok: r.ok, status: r.status, body: await r.text() }))
        .catch((e) => Object.assign(slot, { status: 0, body: String(e) }))
        .finally(() => { slot.done = true; events.poke(); });
      return fetches.length - 1;
    },
    fetch_done: (id) => fetches[num(id)].done,
    fetch_ok: (id) => fetches[num(id)].ok,
    fetch_status: (id) => fetches[num(id)].status,
    fetch_body: (id) => fetches[num(id)].body,
    frame_request() {
      frame = -1;
      // A hidden tab never runs animation frames; the timer keeps the program alive.
      let timer = 0;
      const settle = (ts) => {
        clearTimeout(timer);
        if (frame >= 0) return;
        frame = ts;
        events.poke();
      };
      requestAnimationFrame(settle);
      timer = setTimeout(() => settle(performance.now()), FRAME_FALLBACK_MS * 4);
    },
    frame_time: () => frame,
    // A `raw_` builtin receives its String arguments as the bytes they hold.
    raw_put_pixels(ctx, data, w, h, x, y) {
      ctx.putImageData(new ImageData(new Uint8ClampedArray(data.buffer), num(w), num(h)), num(x), num(y));
    },
    image_new: (w, h) => new ImageData(num(w), num(h)),
    image_set: (img, i, rgba) => {
      const v = num(rgba);
      img.data.set([(v >>> 24) & 255, (v >>> 16) & 255, (v >>> 8) & 255, v & 255], num(i) * 4);
    },
    image_get: (img, i) => {
      const d = img.data, o = num(i) * 4;
      return ((d[o] << 24) | (d[o + 1] << 16) | (d[o + 2] << 8) | d[o + 3]) >>> 0;
    },
  };
}

export async function run(wasm, opts = {}) {
  const args = opts.args ?? [];
  const events = opts.events ?? hostEvents();
  const env = { KAI_THREADS: "1", ...(opts.env ?? {}) };
  const target =
    opts.element ??
    (typeof document !== "undefined" ? document.getElementById("kai-stdout") : null);
  const echo = target ? (text) => { target.textContent += text; } : null;
  const sinks = {
    1: lineSink((l) => console.log(l), echo),
    2: lineSink((l) => console.error(l), echo),
  };
  const encoder = new TextEncoder();
  let memory;
  const view = () => new DataView(memory.buffer);
  const u8 = () => new Uint8Array(memory.buffer);

  const pack = (strings, ptrs, buf) => {
    const dv = view();
    for (const s of strings) {
      const bytes = encoder.encode(s + "\0");
      dv.setUint32(ptrs, buf, true);
      u8().set(bytes, buf);
      ptrs += 4;
      buf += bytes.length;
    }
  };
  const sizes = (strings, countPtr, sizePtr) => {
    const dv = view();
    dv.setUint32(countPtr, strings.length, true);
    dv.setUint32(
      sizePtr,
      strings.reduce((n, s) => n + encoder.encode(s).length + 1, 0),
      true,
    );
    return 0;
  };
  const envStrings = Object.entries(env).map(([k, v]) => `${k}=${v}`);
  const argv = ["kai", ...args];

  const handles = handleTable();
  let pending = new Uint8Array(0);
  const jsctx = {
    handles,
    imports: opts.imports,
    builtins: { kai: { handles: () => BigInt(handles.live()), ...pageBuiltins(events, handles) } },
    decode: (ptr, len) => new TextDecoder().decode(u8().subarray(ptr, ptr + len)),
    bytes: (ptr, len) => u8().slice(ptr, ptr + len),
    take: (dst, n) => u8().set(pending.subarray(0, n), dst),
    result(letter, v) {
      switch (letter) {
        case "U": return undefined;
        case "I": return BigInt.asIntN(64, BigInt(v));
        case "B": return v ? 1 : 0;
        case "H": return handles.put(v);
        case "S":
          pending = encoder.encode(String(v));
          return pending.length;
        default: return Number(v);
      }
    },
  };

  const imports = {
    args_get: (ptrs, buf) => (pack(argv, ptrs, buf), 0),
    args_sizes_get: (c, s) => sizes(argv, c, s),
    environ_get: (ptrs, buf) => (pack(envStrings, ptrs, buf), 0),
    environ_sizes_get: (c, s) => sizes(envStrings, c, s),
    clock_time_get: (id, _precision, out) => {
      const ns = id === 0
        ? BigInt(Date.now()) * 1000000n
        : BigInt(Math.round(performance.now() * 1e6));
      view().setBigUint64(out, ns, true);
      return 0;
    },
    fd_close: () => 0,
    fd_fdstat_get: (fd, out) => {
      if (fd > 2) return ERRNO_BADF;
      const dv = view();
      dv.setUint8(out, 2);
      dv.setUint16(out + 2, 0, true);
      dv.setBigUint64(out + 8, 0n, true);
      dv.setBigUint64(out + 16, 0n, true);
      return 0;
    },
    fd_fdstat_set_flags: () => 0,
    fd_prestat_get: () => ERRNO_BADF,
    fd_prestat_dir_name: () => ERRNO_BADF,
    fd_read: (fd, _iovs, _n, nread) => {
      if (fd > 2) return ERRNO_BADF;
      view().setUint32(nread, 0, true);
      return 0;
    },
    fd_seek: () => ERRNO_SPIPE,
    fd_write: (fd, iovs, n, written) => {
      if (fd !== 1 && fd !== 2) return ERRNO_BADF;
      const dv = view();
      let total = 0;
      for (let i = 0; i < n; i++) {
        const ptr = dv.getUint32(iovs + i * 8, true);
        const len = dv.getUint32(iovs + i * 8 + 4, true);
        sinks[fd].write(u8().slice(ptr, ptr + len));
        total += len;
      }
      dv.setUint32(written, total, true);
      return 0;
    },
    poll_oneoff: () => ERRNO_NOSYS,
    proc_exit: (code) => { throw new ProcExit(code); },
    sched_yield: () => 0,
  };

  const module = await load(wasm);
  const wasi = {};
  for (const { module: m, name } of WebAssembly.Module.imports(module)) {
    if (m === "wasi_snapshot_preview1") wasi[name] = imports[name] ?? (() => ERRNO_NOSYS);
  }
  const hasSuspending = typeof WebAssembly.Suspending === "function";
  let instance;
  const contexts = hasSuspending ? suspension(() => instance, events) : null;
  instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: wasi,
    kai_js: jsImports(module, jsctx),
    kai: contexts ? contexts.imports : { needs_suspension: refuse, wait_until: refuse, ctx_switch: refuse, ctx_exit: refuse },
  });
  memory = instance.exports.memory;

  let code = 0;
  try {
    if (hasSuspending) {
      await Promise.race([WebAssembly.promising(instance.exports._start)(), contexts.failed]);
    } else {
      instance.exports._start();
    }
  } catch (e) {
    if (e instanceof ProcExit) code = e.code;
    else if (e instanceof NoJspi) {
      echo?.(`${NO_JSPI}\n`);
      console.error(NO_JSPI);
      code = 1;
    } else throw e;
  } finally {
    sinks[1].flush();
    sinks[2].flush();
  }
  return code;
}

const [, entry, ...cli] = typeof process !== "undefined" ? process.argv : [];
if (entry && import.meta.url === new URL(`file://${entry}`).href) {
  if (!cli.length) {
    console.error("usage: node kai_host.mjs app.wasm [args]");
    process.exit(2);
  }
  const [wasm, ...args] = cli;
  process.exit(await run(wasm, { args }));
}
