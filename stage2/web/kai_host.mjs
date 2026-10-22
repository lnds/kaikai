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

const refuse = () => { throw new Error(NO_JSPI); };

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
// so the next wait returns at once.
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
    take: () => queue.splice(0),
    wait(deadlineMs) {
      if (latched) {
        latched = false;
        return Promise.resolve(0);
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
        wake = () => done(0);
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
  const needsSuspension = WebAssembly.Module.imports(module).some(
    ({ module: m, name }) => m === "kai" && name === "needs_suspension",
  );
  const hasSuspending = typeof WebAssembly.Suspending === "function";
  if (needsSuspension && !hasSuspending) {
    echo?.(`${NO_JSPI}\n`);
    throw new Error(NO_JSPI);
  }
  let instance;
  const contexts = hasSuspending ? suspension(() => instance, events) : null;
  instance = await WebAssembly.instantiate(module, {
    wasi_snapshot_preview1: wasi,
    kai: contexts ? contexts.imports : { needs_suspension: refuse, wait_until: refuse, ctx_switch: refuse, ctx_exit: refuse },
  });
  memory = instance.exports.memory;

  let code = 0;
  try {
    if (needsSuspension) {
      await Promise.race([WebAssembly.promising(instance.exports._start)(), contexts.failed]);
    } else {
      instance.exports._start();
    }
  } catch (e) {
    if (e instanceof ProcExit) code = e.code;
    else throw e;
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
