# M:N Multi-Threaded Scheduler — Design Formalization

Status: design seed. Author: asu (language architect). Backend of record: LLVM native (`stage2/runtime_llvm.c`), with the portable-C runtime (`stage2/runtime.h`) mirroring every change. Owner decision: ship real parallelism. This document takes a **position on every axis** — it is not a menu.

The one-line thesis: **kaikai already has a shared-nothing, per-fiber-heap, non-atomic-RC actor runtime running on one OS thread. Real parallelism is a *scheduler and runtime-partitioning* change, not a *language* change.** The BEAM lineage kaikai chose in 2026-04 is precisely the lineage that parallelises without a memory-model rewrite — Erlang did exactly this (SMP BEAM, R11B, 2006) on the same shared-nothing substrate. We are walking a paved road.

## 0. The premise, verified against the runtime (not the doc)

Before any decision, the load-bearing facts, read out of `stage2/runtime.h` this session — because the honesty doc's prose and the runtime's actual behavior differ on the single most important axis:

1. **Message send does NOT copy today.** `kai_core_mailbox_send` → `kai_mailbox_push` *transfers the caller's `msg` pointer directly into the mailbox* (callee-consumes, issue #82). The comment is explicit: "transfer the caller's `msg` ref directly into the mailbox." `docs/actors.md` says "messages copied across fibers" — that is a *conceptual* guarantee (shape isolation) that single-thread execution makes free. There is no physical deep-copy on the send path today. **This is the central fact of the whole design.** Under M:N, a cross-thread send would share a non-atomic-RC heap object between two OS threads — a data race on `rc`, and a double-free class bug. The copy that is "free" today becomes the price of admission for parallelism.

2. **The arena stack is a plain global, explicitly flagged as the follow-up.** `runtime.h:2343`: *"The arena stack is a plain global, not `__thread`: kaikai fibers each run to a suspension point on the OS thread that dispatched them, so a region never spans a fiber switch in v1. A per-fiber arena stack is the natural follow-up."* The design already anticipated exactly this lane.

3. **`pthread` is already a baseline dependency.** stage0 includes `<pthread.h>` (`runtime.h:86`) and uses `pthread_mutex_t` / `pthread_cond_t` / `pthread_create` for the 4-worker file-IO pool. So `_Thread_local`, mutex, condvar, and OS threads are already in the "no deps in stage0" envelope — the ANSI-cc constraint is not violated by anything this design needs.

4. **A dual-definition guard already exists for exactly this class of global.** Scheduler and arena globals are already wrapped in `#if defined(KAI_SEPARATE_COMPILATION) … KAI_RUNTIME_OWNER … #else static … #endif` (`runtime.h:2757`). There is *one* owner TU that defines each mutable global. TLS-ifying the runtime is a localized edit to that one guard site per symbol — not a scattered sweep.

5. **The immortal sentinel `rc = INT32_MAX` already exists and already means "RC does not apply to this value."** Regions and singletons use it; `kai_incref`/`kai_decref`/`kai_check_unique` short-circuit on it. This is the mechanism for the "shared immutable, RC-exempt" class (§1).

6. **`kai_deep_copy_out` already exists** — it clones a value out of a region arena onto the RC heap with fresh `rc=1` interior. It is exactly the copy primitive the cross-thread send path needs; it does not need to be invented, only retargeted.

7. **The file-IO pool is the precedent for everything.** `kai_filepool_worker` (4 pthreads), a mutex+condvar FIFO queue, and a self-pipe that wakes the scheduler's `poll()` — this is the reactor-thread-to-scheduler wakeup pattern in miniature, already shipped and gated. The M:N reactor generalizes it; it does not contradict it.

The non-negotiables from the brief hold because of fact (1): non-atomic RC survives *because* the cross-thread boundary gets a physical copy, so no fiber-local object is ever touched by two threads. That is the whole trick. Everything below serves it.

The invariant is load-bearing, so state it in the form the runtime actually has to enforce, and note what it costs:

**No heap value with a non-atomic `rc` may be reachable from two scheduler threads.** Three consequences the original wording left implicit, each of which was a real hole in the runtime until it was closed:

1. **The copy must reach the leaves, not just the spine.** Rebuilding a record while sharing its `Real` / `Char` / `Byte` / fixed-width-Int / `Ref` / `Foreign` fields by pointer leaves exactly the shared `rc` the invariant forbids. The deep-copy walk therefore enumerates every `KaiTag` with no `default:` arm — a sharing fallback is how those tags were admitted in the first place, and without one a new tag cannot join silently. Only two classes may legitimately cross by pointer: values RC does not apply to (`rc == INT32_MAX` singletons) and handles whose rc is atomic by construction (`Fiber[T]`) or whose box is immortal (`Pid`).

2. **At N>1 the copy is unconditional; "is the receiver on my thread?" is not a sound question.** It cannot be answered stably even under a lock. A thief retargets `home_thread` under the *victim's* slot lock, which a sender holds no lock against; and more fundamentally, a decision that is correct when made does not stay correct — whenever Perceus dup'd the value across the send, a same-thread pointer transfer leaves one object owned by two fibers, and a later steal moves one of them to another thread. So the fast path is unsound at any granularity of locking, and the answer is to drop it rather than guard it. This is the BEAM's copy-on-send, and it is what the spawn path already did for the child thunk.

3. **A terminated fiber's result crosses the same boundary as a message.** The fiber wrapper goes on owning `f->result`, and the terminate walk unparks the whole awaiter chain at once, so several awaiters can take it on different threads simultaneously. `await` / `select` therefore copy at N>1 rather than increfing.

> **Cost (2026-07-21, macOS arm64, C backend):** none measurable. `benchmarks/mn-throughput` is unchanged at N=8 (`parallel8` 11.0 → 11.1 ms, `oversub64` 11.1 → 11.1 ms, inside a 1–2% warm-run spread) — but that workload sends `Int`s, which are tagged immediates and never reach the copy at all. A heap-message probe (8 actors × 500 sends of a 40-element list) is also unchanged, 21 → 20 ms at N=8. The reason the unconditional copy is close to free is topological: with work spread over N threads, the fraction of sends that ever took the same-thread fast path is roughly 1/N, so removing it adds ~1/N of the copy work that was already being paid.

The same "classified, but not actually synchronized" defect applies to Class C globals, not just to values — see §1 Class C on `kai_str_intern_table` (publication order under a real lock) and on the `Char` cache, which is immortal only if it is warmed before the first worker exists rather than lazily.

---

## 1. Runtime state partitioning (the taxonomy + the audit)

Three classes. Every mutable file-scope global in `runtime.h` lands in exactly one; the audit's job is to prove the partition is total.

### Class A — `_Thread_local` (per-scheduler-thread)

The hot per-fiber machinery. Each OS scheduler thread owns its own copy; a fiber only ever touches the copy of the thread currently running it (invariant: a fiber is RUNNING on exactly one thread at a time). This is the bulk of the runtime.

- **Arena stack**: `kai_arena_stack[]`, `kai_arena_sp`. (Region values never cross a fiber switch → never cross a thread; a migrating fiber carries no live region — see §10b.)
- **Allocator pools**: `kai_cell_pool*`, `kai_slot_pool_n[]`, `kai_var_block_pool_n[]`, slab allocator state (`kai_slab_atexit` and the slab freelists). A pool is a per-thread free-list; sharing it would require a lock on the hottest path in the runtime. Non-negotiable #1 forbids that. **Per-thread pools, no lock.**
- **Scheduler core**: `kai_ready_head`/`kai_ready_tail`, `kai_parked_count`, `kai_pending_free`, `kai_active_fiber`, `kai_main_fiber` (now one root fiber *per thread*). Work-stealing (§2) makes the ready deque the one structure with a guarded cross-thread entry point, but the *owner* still reads/writes lock-free on its own end.
- **RC trace/debug ledgers**: `kai_rc_live_now`, `kai_rc_live_peak`, `kai_rc_history[]`, `kai_rc_sites[]`, `kai_leaksites[]`, arena counters, prof stack. These are per-thread and **summed at exit** for `KAI_TRACE_RC` reporting (the report loop gains a cross-thread reduction; the counters themselves stay TLS so the hot incref/decref path never synchronizes).
- **RNG**: `_kai_pcg_seeded` + PCG state. Per-thread streams (each thread seeds from a global seed XOR thread-id, so determinism under `KAI_THREADS=1` is byte-identical to today).
- **Per-fiber dispatch/evidence** already lives on `KaiFiber` (not global) — no change; it travels with the fiber.

### Class B — process-global, immutable after init (no sync needed)

Written once during process startup (before any worker thread spawns), read-only forever after. No lock, no atomic — publication happens-before via the thread-create barrier.

- **`kai_g_argc`/argv**, heap limit (`kai_heap_limit_cached`), env-derived config (bench iters, shrink iters, thread count).
- **Static string/interning tables that are populated at compile time and never mutated at runtime**: the `kai_enum_by_tag` catalog, protocol impl-tables, mangled-name tables, `kai_var_names[]` / `kai_slotmask_table` / `kai_varname_table` (codegen-emitted metadata, frozen at program start).
- **The immortal singletons** `kai_singleton_unit/true/false/nil` and the char cache (`kai_char_cache[]`) — they carry `rc = INT32_MAX` already, so even though threads share the pointer, no thread ever mutates the RC. They are Class B *and* immortal, which is why sharing them is sound. Class B says *immutable after init*, and the char cache is only in this class because it is warmed eagerly at scheduler start, before the first `pthread_create`. Warming it lazily on first use put 128 cell writes on whichever worker got there first while its peers were already reading — immortal in rc, but not read-only, which is the half of the Class B contract that actually needs the publication barrier.

### Class C — shared, mutable, needs the immortal treatment OR a lock

The genuinely hard cases: state that is logically process-global, mutated at runtime, and touched by more than one thread.

- **String interning at runtime** (`kai_str_intern_table`): strings are interned lazily at runtime, so the table is a shared mutable cache. **Shipped: interned strings are immortal (`rc = INT32_MAX`) and the table is guarded by a mutex on *insert only*; lookups stay lock-free reads of a grow-only open-addressed table.** The subtlety that makes the lock-free read sound is publication ORDER, not the lock: the bucket's key `cstr` is both its key and its "this bucket is live" flag, so an inserter fills `len` and `value` first and release-stores `cstr` last, and a reader acquire-loads `cstr` before touching the other two. Published before the value, the key lets a concurrent reader match a bucket and return a NULL `value` — on the path of every string literal. Entries are never removed or rewritten, so a bucket a reader observes as published is already final. An interned string is by definition shared and immutable, so `rc = INT32_MAX` is not only sound but *correct* — it removes the string from RC entirely, and RC-exemption is exactly what lets it cross threads without atomics. This is the same trick as regions, reused. If runtime interning turns out to be rare (audit will tell), the simplest sound answer is **per-thread intern tables** (Class A) accepting duplicate string bodies across threads — dedup is a memory optimization, not a correctness property.
- **Handler-id allocation** (`kai_next_handler_id`): a monotonic counter minted at handler install. **Position: atomic fetch-add.** It is off the hot path (once per handler install, not per op) so an atomic is free here.
- **Reactor registration tables** (waiter lists, timer wheel): see §4 — owned by the reactor thread, mutated via message, not shared.
- **Signal-installed flags** (`kai_sigsegv_installed`, `kai_signal_subscribed_init`): process-wide, set once under a `pthread_once`. Class B-ish with an init barrier.

### The audit — the treacherous axis stdout-diff cannot catch

A missed global that *should* be TLS but stays shared is a data race: it passes every functional test under `KAI_THREADS=1` (single thread, no contention) and corrupts silently under load. stdout-diff is blind to it because the corruption is nondeterministic and often benign until it isn't. **Four independent gates, because no single one is sufficient:**

1. **Mechanical enumeration + classification, checked in CI.** A script (`tools/runtime-global-audit`) greps every file-scope mutable definition in `runtime.h` / `runtime_llvm.c` and asserts each appears in an explicit allow-list annotated with its class (`/* @tls */`, `/* @immutable */`, `/* @shared-locked */`, `/* @immortal */`). A new unannotated global **fails CI**. This makes the partition *total by construction*: you cannot add a global without declaring which class it is. This is the primary defense — it converts "did we miss one?" from a judgment call into a build error.
2. **Thread-local hoist gate over the hot runtime bitcode** (`tools/tls-hoist-gate.sh`). Classification (1) says *which* globals are per-thread; this says *where their addresses may be resolved*. The P2 bitcode is merged into the program module before O2, so its functions inline into emitted kaikai frames — and those frames span parks, after which the fiber may be running on another OS thread. Every thread-local the bitcode references must appear in `tools/tls-hoist.allow`, classed `accessor` (every function materialising the address is `noinline` and keeps it, so the address is resolved and consumed inside one activation) or `exposed` (tracked debt: an inlinable function materialises it). The gate re-derives the class from the bitcode, so an entry cannot rot into a claim, and it ratchets — closing an exposure forces the promotion. See §10(b) for the invariant it enforces.
3. **ThreadSanitizer over the multi-actor stress suite** (§7). TSAN instruments every memory access and reports a race on any global touched by two threads without a happens-before edge. A mis-classified global (marked immutable but actually mutated) surfaces as a TSAN report. This catches the *dynamic* errors the static audit's classification got wrong.
4. **`KAI_TRACE_RC` cross-thread reconciliation.** With per-thread RC ledgers summed at exit, a global that leaks across the TLS boundary shows up as an alloc/free imbalance in the summed report (the same divergence-detection the arena counters already provide). A value allocated on thread A and freed on thread B without a copy-out will show as a live-count mismatch.

The static audit (1) is the one that *scales* — it is the discipline that keeps the partition total as the runtime grows. The hoist gate (2) covers the axis (1) cannot see: a correctly-classified TLS global whose *address* outlives the activation that resolved it. TSAN (3) is the correctness proof for a given release. RC reconciliation (4) is the belt-and-suspenders for the RC invariant specifically.

---

## 2. Scheduler topology

**Position: classic per-thread work-stealing deques (Chase-Lev), steal granularity = one fiber, actors migrate between messages (not pinned), spawn placement = LIFO-local push with least-loaded steal.**

Justification axis by axis:

- **Work-stealing over a shared global queue.** A single shared ready-queue serializes on its lock and is the textbook scalability wall (this is why Go moved off the single global runqueue to per-P local runqueues, and why Rust's tokio/rayon use work-stealing). Per-thread deques mean the common case (spawn-and-run locally) is lock-free; contention only appears on steal, which is by construction rare (a thread only steals when its own deque is empty). Chase-Lev is the standard lock-free deque: owner pushes/pops one end without atomics-on-the-fast-path, thieves CAS the other end.
- **Steal granularity = fiber.** The fiber *is* the unit of schedulable work; there is no finer grain (kaikai has no data-parallel loop primitive to split). A stolen fiber is a `KaiFiber*` moved from one deque to another — cheap, it's a pointer.
- **Actors migrate, not pinned.** The serial-mailbox guarantee (non-negotiable #2) holds *regardless* of which thread runs a given message, because the guarantee is "one message processed at a time per mailbox," and a mailbox is drained by one fiber which is RUNNING on one thread at a time. Pinning would buy heap locality (the actor's fiber-local heap stays warm on one core's cache) but costs load balance (a hot actor cannot spread, and a thread with all the hot actors starves the others). **Decision: do not pin.** kaikai's per-fiber heaps are small and the cross-thread cost is the message copy (§10a), which we pay anyway; heap-locality-via-pinning is a micro-optimization that trades away the entire point of the lane (load balance across cores). Erlang schedulers migrate processes between message dispatches for exactly this reason. If a specific workload shows pin-wins, a `Spawn` hint (`spawn_pinned`) is a post-v1 additive escape, not a v1 default.
- **Spawn placement = local LIFO push.** A newly spawned fiber goes on the *spawning thread's* deque, LIFO end. This is the work-stealing canonical choice: the freshly-spawned child is likely to share cache-warm data (the closure, the arguments) with the parent, and LIFO keeps the parent-child pair on the same core until a thief with nothing to do pulls the child. Round-robin placement would actively destroy this locality; least-loaded-on-spawn requires reading every thread's queue depth on the hot spawn path (a synchronization the local-push avoids). **Load balancing is the thief's job, not the spawner's** — this is the core insight of work-stealing and why it beats work-*sharing*.

The one cross-thread-mutable structure is the deque's thief end; everything else in the scheduler stays TLS (§1 Class A).

---

## 3. Mailboxes

**Position: keep the existing intrusive linked-list mailbox with waiter chains; guard cross-thread push with a per-mailbox mutex; wake the destination scheduler via its reactor self-pipe (the mechanism already shipped for the file pool).** Do NOT reach for a lock-free MPSC queue in v1.

Reasoning by profile:

- **Messages are copied and medium-sized** (§10a): a cross-thread send does a `kai_deep_copy_out`-class deep copy into the *destination's* heap, then a pointer-enqueue under the mailbox lock. The copy dominates the cost by orders of magnitude; the lock hold is a few instructions appending one node. A lock-free MPSC (e.g. Vyukov's intrusive MPSC) optimizes the enqueue, which is the part that *isn't* the bottleneck. **The copy is the cost, not the lock.** Adding a lock-free queue here is optimizing the wrong end and buys a class of ABA/memory-reclamation subtlety for no measured win.
- **Same-thread send stays exactly as today** — no lock, no copy needed *if* sender and receiver are provably on the same heap. But proving that on the send path is fragile (a fiber can migrate). **Simplest sound v1: always copy on send when the destination mailbox is owned by a fiber not currently on this thread; enqueue-by-pointer only when same-thread.** The cheap same-thread fast path is a runtime check (destination fiber's current-thread id == my thread id); mismatch → copy. This keeps the single-thread `KAI_THREADS=1` path byte-identical (always same-thread → never copies → today's behavior).
- **Wakeup mechanism = destination reactor's self-pipe / eventfd.** When thread A pushes to a mailbox whose owner is PARKED on thread B's reactor, A writes one byte to B's wakeup pipe. This is *exactly* the `kai_filepool_pipe` pattern already in the tree — the file-pool worker writes a byte to wake the scheduler's `poll()`. We generalize: every scheduler thread has its own wakeup pipe in its own `poll()` set. Futex-on-Linux / condvar would work too, but the self-pipe unifies with the reactor's existing `poll()` loop (one wait primitive, not two) and is already portable across macOS/Linux. **Decision: per-thread wakeup pipe folded into that thread's poll set.** (An `eventfd` on Linux is a strictly-better self-pipe; use it under `#ifdef __linux__` as a perf refinement, self-pipe as the portable floor.)

---

## 4. Reactor

**Position: keep a single dedicated reactor thread (evolve the current in-scheduler `poll()` into its own thread); it owns all I/O waiter tables and the timer wheel; on readiness it hands the ready fiber back to the *owning* scheduler thread by enqueue + wakeup-pipe.** Do NOT go to per-thread reactors in v1.

- **Why one reactor thread, not per-scheduler poll.** The waiter tables (socket read/write lists, timer wheel, stdin/signal singletons) are today singletons in the scheduler thread. Sharding them per-scheduler-thread means a socket fd registered by a fiber that then migrates has its waiter on the wrong thread's poll set — you'd need to migrate poll registrations on every fiber steal. That is complexity with no payoff: I/O readiness is not CPU-bound, so one thread running `poll()`/`epoll`/`kqueue` over all fds is not a bottleneck (this is the reactor design of every production async runtime's I/O thread; the CPU work is on the scheduler threads, the reactor just demultiplexes readiness). **One reactor thread, all fds, owns the timer wheel and the singleton stdin/signal waiters.**
- **Migration path from today.** Today `poll()` runs *inline* in the scheduler loop on the one thread. Step 1 (still single-scheduler): extract the `poll()` loop into `kai_reactor_thread`, and have it enqueue-ready + wake via the pipe that the scheduler already drains. This is behavior-preserving under `KAI_THREADS=1` (one scheduler + one reactor = same interleaving, verified by byte-parity). Step 2 (multi-scheduler): the reactor's "enqueue ready fiber" now routes to *the scheduler thread that owns that fiber* (each `KaiFiber` gets a `home_thread` field, set at park time) and wakes that thread's pipe.
- **The file-IO pool (4 workers) folds in unchanged.** It already offloads blocking `fopen`/`read` to worker threads and wakes the scheduler via `kai_filepool_pipe`. Under M:N its wake target becomes the owning scheduler thread instead of "the" scheduler thread. This is the *proof of concept* that the whole reactor-hands-back-to-scheduler pattern works — it already ships.

---

## 5. Cancel / Link / Monitor cross-thread

**Position: all three become system messages delivered through the mailbox / a per-fiber atomic flag; Cancel stays cooperative; the only atomicity needed is on the cancel-request flag and the link/monitor list mutation.**

- **Cancel.** `Spawn.cancel(target)` today sets `target->cancel_requested` and the target checks it at yield points. Cross-thread, the writer (thread A) and reader (thread B, the target's own thread at its next yield) touch one `int`. **Position: make `cancel_requested` / `cancel_delivered` `_Atomic int` (relaxed store on request, acquire load at the yield-point check).** This is the *only* per-fiber field that goes atomic, it is off the RC hot path entirely (checked at yield points, not per allocation), and it preserves cooperative semantics exactly — the target still only acts on it at a yield boundary. `pthread_kill`-style preemptive cancel stays deferred (pinned decision, Orongo territory), unchanged.
- **Link / Monitor.** These mutate intrusive lists (`linked_head`, monitor nodes) on the *peer* fiber, which may be on another thread. Two options: (a) atomic-CAS list insertion, or (b) route link/monitor establishment and teardown as **system messages** to the peer's mailbox, so only the peer's own thread mutates its own lists. **Position: (b), system messages.** It reuses the mailbox (already the cross-thread transport), keeps every fiber's link/monitor lists single-writer (the owning thread), and matches Erlang's model (link/monitor are signals, not shared-memory writes). The propagation walk on termination (setting `cancel_requested` on linked peers) sends a cancel *message* to each peer rather than reaching into the peer's struct — which composes with the atomic cancel flag above (a linked-peer death enqueues a cancel-request that the peer's thread observes at its next yield).
- **What genuinely needs atomicity:** the cancel flag (one atomic int per fiber) and the handler-id counter (§1 Class C, atomic fetch-add). That is the *entire* atomic surface. Everything else routes through the mailbox, which is already the sanctioned cross-thread channel. **Non-negotiable #1 (RC non-atomic) is untouched** because none of this is on the object-RC path.

---

## 6. Context switch

**Position: keep `ucontext` for v1; the M:N switch to asm-context is a *separable, independently-gated* follow-up, NOT a prerequisite.**

- `ucontext` is thread-agnostic: `swapcontext` saves/restores a `ucontext_t` that lives in the `KaiFiber` struct (heap, not thread-owned). Nothing in `swapcontext` binds a context to the thread that created it — it saves the CPU register file and stack pointer, both of which are position-independent. So a fiber's `ucontext_t` can be resumed on a *different* thread than the one that suspended it, which is exactly what migration needs. (Caveat verified in §10b.)
- The known problems with `ucontext` are **performance** (~1-2 µs/switch, macOS-deprecated) not **correctness under threads**. Those are the same reasons the asm-switch was already listed as an Orongo item — and they are orthogonal to M:N. Shipping M:N on `ucontext` first, then swapping the switch implementation under it, is two independently-verifiable lanes. Coupling them would gate parallelism (the payoff) on a context-switch rewrite (a perf refinement) for no reason.
- **Decision for v1: `ucontext`. The asm-switch is its own lane, gated on its own microbenchmark, landed whenever — the M:N correctness does not wait on it.**

One real interaction to pin: `ucontext_t` on some platforms saves the signal mask. Under a dedicated reactor thread that handles signals (§4), the per-scheduler-thread signal mask must be set once at thread creation (block the reactor-handled signals on scheduler threads) so a migrating fiber does not carry a stale mask that re-enables a signal on the wrong thread. Set the mask at scheduler-thread init, before any fiber runs; `swapcontext` then preserves the intended (blocked) mask uniformly.

---

## 7. Rollout / gating

**Position: `KAI_THREADS=N` env var, default `1`, byte-identical to today at `N=1`; flip default to `N=ncpu` only after the full gate ladder is green; new CI tiers for TSAN, messaging stress, and test determinism.**

- **`KAI_THREADS=1` is the compatibility floor and the primary regression gate.** At `N=1`: one scheduler thread + one reactor thread, always-same-thread sends (never copy, pointer-enqueue as today), no steals. This must be **byte-identical** to the pre-lane runtime on every existing fixture. It is the proof that the partitioning did not change single-thread semantics. This is the gate that lets the lane land incrementally without risking the shipped runtime.
- **New CI:**
  - **TSAN tier** (path-gated on `runtime*.c/.h`, `stdlib/**` concurrency, `examples/effects/**`): builds the runtime with `-fsanitize=thread`, runs the multi-actor stress suite at `KAI_THREADS>1`. **Zero races is the merge gate** — this is the one that catches a mis-partitioned global (§1 audit gate 2).
  - **Messaging stress** (`examples/concurrency/` new): N producers → M actors, high message volume, at `N=ncpu`; asserts no lost/duplicated/corrupted messages and clean RC ledger at exit.
  - **Determinism guard**: every existing `kai test` fixture must produce identical output at `KAI_THREADS=1` and `KAI_THREADS=4`. A test whose output depends on scheduling order is either a bug in the test or a missing serialization point; this gate finds both. (Tests that *intentionally* observe parallelism opt out via a marker.)
  - **rb-tree RC regression** at `KAI_THREADS=1`: the non-atomic-RC hot path must show **zero** slowdown vs pre-lane (the 1.8× RC-density sensitivity measured this week is the canary — if TLS-ification or a stray atomic crept onto the RC path, this bench moves). This is how we *prove* non-negotiable #1 held.
- **Default flip is its own PR**, after all four tiers are green across a soak, so it is trivially revertible (`KAI_THREADS=1` restores the old world with one env var).

---

## 8. Memory model — what document pins it, and does it touch the surface

**Position: the memory model is pinned in a new `docs/memory-model.md` (or a major section of `docs/actors.md` + `docs/structured-concurrency.md`); it does NOT touch the edition surface. Confirmed: no surface change.**

The model, stated precisely:

- **The only inter-fiber communication is message passing.** A message send establishes a happens-before edge from the send to the corresponding receive, realized physically by a **deep copy into the receiver's heap** (cross-thread) under the mailbox lock (a release/acquire pair). After the copy, sender and receiver share no memory. There is no shared mutable state between fibers — non-negotiable #4, now *physically enforced* by the copy rather than merely conceptually true under single-thread.
- **Within a fiber, execution is sequential and RC is non-atomic** — unchanged, non-negotiable #1.
- **Immortal values (`rc = INT32_MAX`: singletons, interned strings, region-escaped-and-frozen data) may be shared read-only across threads** because they are excluded from RC entirely; there is no write to race on.

**Why the surface is untouched (refuting the possibility of a surface change):** `spawn`/`await`/`select`/`cancel`/`nursery`/`send`/`receive`/`self` are all *effect operations* whose *types and syntax do not change*. The parallelism is entirely in how the `Spawn`/`Actor` runtime dispatches those ops across OS threads. A user's source that compiles and runs today compiles and runs identically — it just uses more cores. There is no new keyword, no new type, no new annotation, no coloring, no `Send`/`Sync`-style marker trait (kaikai's shape-isolation makes the copy unconditional, so there is nothing for the type system to prove — this is precisely the Erlang advantage over Rust's `Send + Sync`). Therefore: **no edition bump, no Tier-1 stability event.** It is a pure runtime change under a stable surface — exactly the category CLAUDE.md §4 says we iterate freely on ("runtime, codegen, RC discipline"). The `KAI_THREADS` env var is not surface (it's a deployment knob, like a GC tuning flag), so even it does not touch the stability contract.

The one honest caveat to document: a program that *accidentally depended on single-threaded execution order* of independent fibers (e.g. two unsynchronized actors whose interleaving happened to be deterministic) may observe different interleavings at `N>1`. This is not a compatibility break — the language never promised a deterministic interleaving of concurrent fibers (that would defeat concurrency) — but the doc must state it plainly, and the determinism CI gate (§7) surfaces any test that relied on it.

---

## 9. Phases with measurable gates

The sequence, each phase independently landable and gated by a **binary** check (byte-parity / TSAN-clean / measured-speedup / bench-no-regression). Order chosen so that soundness dependencies (the copy-on-cross-thread-send, the TLS partition) land *before* the thing that would expose them (multiple threads).

### F0 — Runtime partitioning, `N=1` byte-identical
Scope: implement the §1 taxonomy. TLS-ify Class A, `pthread_once`/immutable Class B, immortal-or-lock Class C. Add the static global-audit CI gate (§1 gate 1). Extract the reactor `poll()` loop into its own thread (§4 step 1). Still **one scheduler thread**.
- **Gate:** byte-identical output on every existing fixture; the static audit passes (every global classified); `KAI_TRACE_RC` ledger reconciles across the now-two-thread (scheduler + reactor) process. rb-tree RC bench: zero regression.
- **Why first:** this is the sound foundation. It changes *where* state lives without changing *behavior*. If it can't be made byte-identical, nothing downstream is safe.

### F1 — M:N scheduler + cross-thread-safe mailboxes, `KAI_THREADS` opt-in
Scope: per-thread work-stealing deques (§2, Chase-Lev), the copy-on-cross-thread-send path (§10a / §3), per-thread wakeup pipes, `home_thread` on `KaiFiber`. Cancel flag → atomic; link/monitor → system messages (§5). Default still `N=1`.
- **Gate:** at `KAI_THREADS=4`, a multi-actor CPU-bound bench shows **measured speedup ≥ (say) 3× on 4 cores** (the number the owner picks; the point is it *scales*, refuting kaikai-vs-rust case 3's "actor is serial"). TSAN-clean on the stress suite. At `KAI_THREADS=1`, still byte-identical (the copy path is inert same-thread). rb-tree RC bench at `N=1`: still zero regression (proves the RC path stayed non-atomic — the atomic cancel flag is nowhere near it).
- **Why here:** this is where parallelism first exists and where the copy-boundary (the soundness linchpin) is exercised under real contention. TSAN on this phase is the moment of truth for the §1 partition.

### F2 — Multi-scheduler reactor
Scope: reactor's ready-handback routes to the owning scheduler thread + wakes that thread's pipe (§4 step 2); file-pool wake target follows the fiber; timer wheel and stdin/signal singletons stay reactor-owned.
- **Gate:** an I/O-heavy multi-fiber bench (N concurrent socket clients + M CPU actors) at `N=ncpu` shows both I/O progress and CPU parallelism (neither starves). TSAN-clean including the reactor-to-scheduler handoff. Determinism gate on all `kai test` fixtures.
- **Why after F1:** the reactor handoff is only interesting once there are multiple schedulers to hand back *to*.

### F3 — Default `N=ncpu`, case-3 won
Scope: flip `KAI_THREADS` default to `ncpu`. Update `docs/fibers-honesty-targets.md` (Tier 3 multi-thread → shipped), `docs/memory-model.md`, and the kaikai-vs-rust comparison (case 3 flips from "kaikai's actor is serial" to "kaikai parallelises the actor across cores with no `Send`/`Sync` cost").
- **Gate:** the full ladder green under soak; the flip is one-line-revertible via `KAI_THREADS=1`. Publish the multi-core actor bench as the case-3 artifact.

Each phase's gate is binary — byte-parity, TSAN-clean, a speedup number, a no-regression number — so a phase that goes wrong *shows* and reverts. No phase depends on a time estimate; each depends only on the prior phase's soundness gate being green.

---

## 10. The attacks — answered before signing

### (a) What does cross-thread message copy break vs today (destination allocator vs sender)?

Today send transfers the pointer (§0 fact 1) — zero copy, and the object lives on the *shared* heap that happens to be one thread's. Cross-thread, the object must be **deep-copied into the destination fiber's heap** so it is RC'd by the destination's (non-atomic) RC and never touched by the sender's thread again. The primitive exists: `kai_deep_copy_out` already deep-copies a value onto a fresh-`rc=1` heap allocation; retarget it to allocate from the *destination thread's* pools. **The trap:** allocation must use the destination's TLS pools, but the copy runs on the *sender's* thread (the sender is the one holding the message). So the sender's thread bump-allocates into the *receiver's* pool — a cross-thread pool write, which violates the "pools are per-thread, no lock" rule (§1 Class A). **Resolution: the copy allocates from the *sender's* heap (producing an rc=1 tree owned by the sender momentarily), the pointer is enqueued, and ownership transfers to the receiver at pop; the receiver's *first touch* is what matters for its RC, and since the copied tree is rc=1 and single-owner, the receiver simply adopts it.** No cross-thread pool write: the sender allocates on its own pool, the receiver frees on its own pool (the tree migrates like a fiber does — see 10b). The subtlety: a tree allocated on thread A's pool and freed on thread B's pool means B's `free`-list grows with A's blocks. This is fine for a *slab/pool* only if the pool is a global-freelist-per-size (blocks are interchangeable) — **audit item: confirm the slab allocator's free path is size-classed and pool-agnostic on free, or make freed blocks return to the freeing thread's pool (which they naturally do if free pushes onto the caller's TLS free-list).** The latter is the standard answer (tcmalloc/jemalloc thread-caches do exactly this: allocate local, free-to-local, periodic rebalance). **Position: free-to-local-TLS-pool; the copied message tree is the migrating unit, same discipline as a migrating fiber.**

### (b) Is it legal to migrate a `ucontext`/fiber between threads on macOS/Linux? Stack-referencing-C-values?

- **The `ucontext_t` itself: yes, portably.** It is a saved register file + stack pointer, both position-independent; it lives in the heap-allocated `KaiFiber`, not in TLS. `swapcontext` on thread B restoring a context saved on thread A is defined behavior on both Linux (glibc) and macOS (the deprecated-but-present impl) — the context does not encode a thread identity. Goroutines, Erlang processes, and every M:N runtime migrate stacks between OS threads; this is well-trodden.
- **The real trap is not the context, it's what the fiber's C stack *references*.** If a fiber's suspended C stack holds a pointer to a `_Thread_local` variable (e.g. it cached `&kai_arena_sp` or a pool pointer before yielding), that pointer now refers to the *wrong* thread's TLS after migration. **Mitigation: a TLS address must never outlive the activation that resolved it.** Note that writing the source by *value* at point of use (`kai_arena_sp++`, never `int *p = &kai_arena_sp` spanning a `swapcontext`) does **not** buy this: the address capture happens in the optimiser, not in the source. `llvm.threadlocal.address` is `speculatable memory(none)`, so hoisting, sinking and CSE-ing it across an opaque call — including the call that parks — is legal, and clang does it. The only construction that holds is a `noinline` function that materialises the address and keeps it, because thread identity is constant for the duration of one activation. **Gated by `tools/tls-hoist-gate.sh`** over the hot bitcode (§1, gate 2): every referenced thread-local is classed `accessor` (verified: only `noinline` materialisers, address never returned) or `exposed` (tracked debt), and a new unlisted one fails the build. The C stack may also hold `ucontext`-saved callee-saved registers pointing into the fiber's *own* heap stack — those migrate *with* the fiber (the stack is heap, carried in `KaiFiber.stack_base`), so they stay valid. Only TLS-address-capture is the hazard.
- **Region values (§1) never span a yield** (the pinned region invariant), so a migrating fiber carries no live arena pointer — the arena TLS is empty at every suspension point. This is why arena-stack-as-TLS is sound under migration.

### (c) Timer wheel and singleton waiters (stdin/signal) under N threads?

- **Timer wheel: reactor-owned (§4), single writer.** A fiber on any scheduler thread that calls `sleep` sends a "register timer" to the reactor (or, cheaper, the park-timer path enqueues into a reactor-owned structure guarded by the reactor's own lock, touched only at park/fire). One thread (the reactor) owns the wheel; no cross-scheduler races. Fire routes the ready fiber back to its `home_thread`.
- **stdin/signal singletons: reactor-owned, and the "one reader" contract already enforced.** Today stdin/signal already panic on *two concurrent readers* ("serialize via an actor"). That contract is *unchanged and now more important*: exactly one fiber may park on stdin/signal, and it does so via the reactor thread. Under N schedulers this is still a singleton on the reactor — no new race, the existing panic-on-double-reader is the guard. Signal delivery: the reactor thread owns the signal self-pipe and the `sa_handler` (async-signal-safe write to the pipe); scheduler threads block the handled signals at init (§6) so signals land only on the reactor. This is the standard "dedicated signal thread" pattern and it *simplifies* under a dedicated reactor thread (today the signal handling shares the scheduler thread; giving it its own thread is cleaner).

### (d) `Ffi` blocking calls — need an FFI thread pool?

**Yes, and the precedent is already in the tree.** A blocking `extern "C"` call today blocks the one scheduler thread; under M:N it would block *one* scheduler thread, stalling every fiber on that thread's deque (the others keep running, so it's less catastrophic than today, but still wrong — a blocking FFI call should not freeze N fibers' worth of work). **Position: blocking FFI calls offload to a thread pool exactly like the file-IO pool does today** (`kai_reactor_run_in_pool`). The file pool *is* an FFI-offload pool already — it runs blocking `fopen`/`read` on worker threads and parks the calling fiber. Generalize it: an FFI call annotated (or heuristically detected) as potentially-blocking runs on a pool worker; the calling fiber parks; on completion the worker wakes the fiber's home scheduler. **The open design question is which FFI calls offload** — offloading *every* FFI call adds a thread-hop to cheap pure-C calls (e.g. a `sin()`). Position: **default to inline (fast), offload only FFI declared blocking** via an attribute on the `extern` decl (`extern "C" #[blocking] fn read(...)`), mirroring how the runtime already knows file ops block. A pure/fast FFI call runs inline on the scheduler thread (the common case, zero overhead); a declared-blocking one offloads. This is an additive surface (`#[blocking]` on extern) — but note it *is* a small surface addition, so it must clear the Tier-1 bar; it's sound (worst case of mislabeling: a blocking call runs inline and stalls one thread, same as today's whole-runtime stall but narrower) so it can ship as a refinement after F1 without gating parallelism.

---

## Surviving risks (what I did not fully close)

1. **The slab allocator's free-path pool-agnosticism (10a).** The design *requires* that a block allocated on thread A and freed on thread B returns cleanly to B's TLS free-list (jemalloc-style thread-cache with periodic rebalance). If the current slab allocator assumes alloc-thread == free-thread anywhere, that assumption must be found and removed. This is a runtime-audit item for F0/F1, not a design unknown — the answer is known (free-to-local), but the *current code's* conformance is unverified this session.

2. **`#[blocking]` FFI classification is a genuine (small) new surface** (10d). It clears soundness trivially but it is a language-surface addition and must pass the Tier-1 bar on its own merits. It is decoupled from the parallelism lanes (F1 ships without it; FFI just stalls one thread until it lands), so it is not a blocker, but it is real added surface the owner should sign off on separately.

3. **Runtime string interning across threads (§1 Class C)** — I gave two sound answers (immortal+locked-insert, or per-thread-tables-accepting-dup) but did not pick one, because the choice depends on *how much* runtime (not compile-time) interning actually happens, which the audit measures. Both are sound; it's a perf choice pending data. Flagged, not closed.

4. **macOS `ucontext` deprecation risk over the lane's lifetime.** `ucontext` works for M:N today but is deprecated on macOS and could be removed. The asm-switch follow-up (§6) de-risks this, but if Apple pulls `ucontext` before the asm-switch lands, F0 could be forced to co-land the asm-switch. Low probability, named so it is not a surprise.

5. **Determinism gate false-positives (§7).** Some existing tests may have latent order-dependence that only surfaces at `N>1`. The gate *finds* them (good) but each is a small investigation (is it a test bug or a real missing serialization?). This is expected churn in F1/F2, not a design flaw — but it is real work the phases will surface, not avoid.

None of the five is a soundness hole in the design; four are audits-of-current-code or perf-choices-pending-data, and one (`#[blocking]`) is a decoupled small-surface sign-off. The core — copy-on-cross-thread-send preserving non-atomic RC — is sound and is the same thing Erlang shipped in 2006.
