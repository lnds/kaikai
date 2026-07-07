# Native backend perf — bisection measurement (#1104)

Read-only measurement spike, 2026-07-07, kaikai `37857e7d` (v0.99.0),
Darwin arm64, `cc -O2` / libLLVM-18 static, native kaic2 rebuilt fresh
(`touch stage2/main.kai && gen-runtime-bc.sh && make -C stage2 KAI_LLVM=1
kaic2`). No code changed. Every number below is from the **real binary
kaic2 emits**, measured with `/usr/bin/time -l` (instructions retired +
wall) and `otool -tV` / `KAI_NATIVE_DUMP_IR` for static call counts — no
hand-reconstructed IR, no `opt`/`clang` re-optimisation of a dump (the
method error of the first #1083 pass).

## 1. What was already established (prior docs)

- **#1083 closed the native-vs-C codegen gap partway.** The arc shipped
  four landed fixes, all present in HEAD and verified active here:
  `#1088` stamp host `target-features` on emitted fns so the inliner
  folds the runtime ops (`stage2/runtime.h:14193`
  `kai_llvm_stamp_host_features`, called at `runtime.h:14282` after the
  bitcode link, before O2); `#1092` variant fast-entry construction;
  `#1100` i64-inline variant slots on **construction**
  (`kaix_variant_masked` over a raw `[n x i64]` word buffer,
  `emit_native_ops.kai:720`); `#1102` `KProjBorrow` reads a known-pointer
  slot as a direct `.ptr` borrow.
- **The residual named by both `benchmarks/rb-tree/README.md` and the
  #1104 issue**: the `kaix_*` shim call boundary (native keeps
  `%KaiValue` opaque and routes cell access through external `kaix_*`
  where the C backend inlines the whole runtime in one TU) **plus** the
  deferred kind-1 Int raw **binder read** (reverted in #1102 because it
  SIGSEGVs the native self-host gate — the #709 phantom-box class: a raw
  register flowing to a boxed consumer with no re-box border).
- **The Koka-parity plan** (`docs/koka-parity-plan.md`) established that
  the remaining gap **to Koka** is a shared front-end cost (Perceus RC
  residual + descent machinery), not codegen — and #1105 landed its Lane
  1 (donate the arm reuse token across the balance boundary).

This spike does NOT re-measure the Koka gap. It bisects the **native-vs-C
codegen gap** the #1104 brief flagged as the project's largest remaining
wall delta, and quantifies the two named suspects against the real binary.

## 2. Bisection: turn off work sources, watch which ratio survives

Three benches, same rb-tree source (`examples/perceus/rb_tree.kai`), same
front-end, same KIR — only the codegen path differs (C backend vs native
backend). Each measured 3× with `/usr/bin/time -l`, medians below.

| bench | isolates | C insns | native insns | **native/C insns** | C wall | native wall | native/C wall |
|---|---|---:|---:|---:|---:|---:|---:|
| `arith_runtime` | raw Int math, no heap | 14.5 M | 12.7 M | **0.87× (parity)** | 0.50 s | 0.32 s | parity |
| **pure lookup** (20M descents, tree built once, **zero** alloc/RC-diff) | variant **navigation** only | 19.66 G | 39.04 G | **1.99×** | 6.9 s | 8.2 s | 1.19× |
| **full insert** (N=1M, build + RC + balance) | + construction + RC + boxing | 2.248 G | 6.268 G | **2.79×** | 0.35 s | 0.49 s | 1.40× |

Reading the bisection:

- **Scalar arithmetic is at parity** (native 0.87×). The Int-unbox lanes
  (#853/#857/#859) closed it; it is not a suspect. Any native gap on the
  rb-tree is therefore **variant/heap-shaped, not arithmetic-shaped**.
- **Turning off allocation, RC-difference, and balance (pure lookup) does
  NOT collapse the ratio to ~1.0×** — it drops from 2.79× to **1.99×** and
  stops there. So there are **two distinct sources**:
  1. a **navigation-invariant ~2× instruction penalty** present even in a
     pure read-only descent (this is the residual the pure-lookup bench
     isolates);
  2. an **allocation/construction/RC penalty** that lifts 2.0× → 2.8× on
     the full insert.
- **Peak RSS is identical** (47.6 MB both backends at 20M descents), so the
  pure-lookup gap is not a leak or runaway alloc — it is instruction count
  on the same live set.

The pure-lookup 1.99× is the largest *instruction*-count residual, and a
single localised codegen decision — but **the instruction gap is NOT the
wall gap**. §2b measures where the wall actually is.

## 2b. Where the WALL is (vs where the instructions are)

The user prioritises wall-clock, not instruction count. `/usr/bin/time -l`
on this machine reports **both** `instructions retired` and `cycles
elapsed`, so CPI (cycles ÷ instructions, its inverse IPC) is directly
measurable — and CPI is the discriminator: a **high** CPI means the wall is
lost to *stalls* (cache misses, branch mispredicts, serialized data
dependencies); a **low** CPI means the extra instructions are cheap and
well-pipelined, so wall ≈ cycles ≈ instructions ÷ IPC and the only wall
lever is fewer instructions.

Measured, this machine, `37857e7d`, interleaved medians:

| bench (what it isolates) | backend | wall | insns | cycles | **CPI** | IPC | native/C wall | native/C cyc |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| **insert** (build+RC+balance) | C | 0.35 s | 2.25 G | 1.60 G | 0.71 | 1.41 | | |
| | native | 0.48 s | 6.27 G | 2.17 G | **0.35** | **2.89** | 1.37× | 1.36× |
| **pure lookup** (20M descents) | C | 7.46 s | 19.74 G | 33.10 G | 1.68 | 0.60 | | |
| | native | 9.04 s | 39.15 G | 40.17 G | **1.03** | **0.97** | 1.21× | 1.21× |
| **tag-walk** (`rb_size`, tag dispatch, NO key read) | C | 0.150 s | 1.75 G | 0.67 G | 0.39 | 2.60 | | |
| | native | 0.160 s | 2.16 G | 0.73 G | **0.34** | **2.95** | 1.07× | 1.09× |
| **key-sum** (reads int key every node, no branch) | C | 0.150 s | 1.75 G | 0.69 G | 0.40 | 2.53 | | |
| | native | 0.170 s | 2.60 G | 0.78 G | **0.30** | **3.34** | 1.13× | 1.12× |
| **red-count** (boxed-Bool color dispatch: `kaix_tag_eq`+`kaix_truthy`) | C | 0.160 s | 2.19 G | 0.71 G | 0.33 | 3.08 | | |
| | native | 0.170 s | 3.03 G | 0.79 G | **0.26** | **3.84** | 1.06× | 1.11× |

Three facts that settle the wall question:

1. **Wall tracks cycles, not instructions — exactly.** In every bench,
   `native/C wall ≈ native/C cycles` (insert 1.37 vs 1.36; lookup 1.21 vs
   1.21; key-sum 1.13 vs 1.12; red-count 1.06 vs 1.11). The *instruction*
   ratio (2.79×, 1.98×) grossly overstates the wall gap. Cycles are the
   honest wall proxy; instructions are not.

2. **Native never stalls more than C — it stalls LESS.** Native CPI is
   *lower* than C in every bench (insert 0.35 vs 0.71; lookup 1.03 vs 1.68;
   red-count 0.26 vs 0.33). Native runs its large instruction stream at
   2.9–3.8 IPC — the boxed-representation ops are independent loads the
   core pipelines beautifully. There is **no cache/branch/dependency
   stall** to attack: page reclaims (3702 vs 3704) and peak footprint
   (57.5 MB both) are identical, and the low CPI rules out a
   mispredict/miss regime. The C backend, with its dense pointer-chasing
   descent, actually has the *higher* CPI — it just runs so few
   instructions total that its cycle count still wins.

3. **My own §4 falsification suspect — the boxed-Bool `kaix_tag_eq`
   dispatch — is REFUTED by measurement.** The `red_count` bench exercises
   `kaix_tag_eq` + `kaix_truthy` on every node (2 each, verified in the
   IR). Its native CPI is **0.26 — the lowest of any bench**, better than
   plain tag-walk (0.34). The boxed-Bool color compare does not serialize;
   it pipelines *better* than plain dispatch. `kaix_tag_eq` is not a wall
   lever.

**Marginal attribution — what the key-read actually costs in cycles.**
Key-sum and tag-walk both do 20M node-visits; key-sum adds one int-key read
per node, tag-walk adds none. The native delta:

- native: **+0.44 G instructions, +0.045 G cycles** → marginal 0.10
  cyc/insn. The boxed key-read (`kaix_variant_arg` + `kaix_int_field` +
  `kaix_int`) adds instructions that cost **≈ 1 cycle per 10** — nearly
  free on the pipeline.
- C: +0.00 G instructions, +0.019 G cycles — the inline `.i64` load is
  genuinely free.

So the kind-1 raw binder (the §3/§4 lever) removes ~0.44 G native
instructions on this walk but only ~0.045 G cycles (≈ 6 % of the walk's
cycles) → a **~0.02 s wall move on the 20M-visit walk**. On the full insert
it applies in the descent + `balance_*` and would recover a proportional
slice of native's 0.57 G excess cycles — a wall move of **~0.03–0.05 s**
(0.48 → ~0.44), not toward C's 0.35.

**The honest verdict on the wall.** Native's wall gap is *entirely* its
larger cycle count, and the cycle count is `instructions ÷ IPC` with an
*already-excellent* IPC. There is **no stall-based lever** — nothing to fix
in layout, branch structure, or dependency chains, because native is
already the lower-CPI backend. The only way to move native wall is to
shrink the instruction stream, and because IPC is so high the payoff is
sub-proportional: the 2.79× instruction gap is a **1.37× wall gap**, and a
lever that halves the *excess* instructions buys well under half the
*excess* wall. The kind-1 raw binder is the cleanest single instruction
reducer (§3–4) and it does move wall — but modestly (~0.03–0.05 s on the
1M insert). A user who feels 0.48 s vs 0.35 s will feel it drop to ~0.44 s,
not to parity. That is the truth to hold: **native is not stall-bound;
there is no dramatic wall lever, only broad instruction reduction with
IPC-diluted payoff.**

## 3. Suspect (a) — the shim boundary, quantified: it is the READ path

The insert-bench README frames the shim boundary as "native routes cell
access through an external `kaix_*` shim." After the #1088 target-features
stamp, `default<O2>` **does** inline the `kaix_*` bodies (verified: the
pure-lookup descent loop has **no `bl kaix_*` per iteration** — the
field/tag reads are inlined to `ldr`/`ldrh`). So the residual is NOT
un-inlined calls. It is what the inlined bodies expand to.

Disassembly of the pure-lookup descent, ONE node step (`otool -tV`,
instruction histogram):

| | C backend | native backend |
|---|---:|---:|
| `ldr` (loads) | 6 | **12** |
| `cmp` | 6 | 9 |
| `mov` | 2 | 8 |
| `ldrh` (variant-header reads) | 1 | **3** |
| `ubfx` (color/tag bitfield extract) | 0 | **2** |
| `cmn` (tagged-immediate box-guard) | 0 | **2** |
| whole-`_main` code size (lines) | 895 | 1797 (**2.0×**) |

The native pre-opt IR names the exact per-node divergence. In `rb_get`,
each descent step reads the key slot as:

```
%14 = call ptr @kaix_variant_arg(ptr %13, i32 2)   ; box-borrow the key slot
%27 = call i64 @kaix_int_field(ptr %26)            ; UNBOX it to i64
%28 = icmp slt i64 %25, %27                        ; compare raw
```

— a **two-call box-borrow-then-unbox** per key read (7 `kaix_int_field`
unboxes + 3 `kaix_int` re-boxes in one `rb_get`). The C oracle reads the
same key in a **single inline load**:

```c
int64_t kair_k0 = kai_var_slots(_scr)[2].i64;   // raw .i64, one load
_r = (kair_k < kair_k0) ? ... : ...;             // raw compare
```

(source: `rb_get` in the emitted C, `kai_var_slots(_scr)[2].i64`). Even
after O2 inlines `kaix_variant_arg`/`kaix_int_field`, the two-step dance
survives in the instruction stream as the extra `ldrh` header read + `ubfx`
extraction + box-guard — the measured 2× loads/moves per node.

**This is not the call boundary per se; it is the boxed representation of
the read binder.** The native emitter *has* a raw-read path
(`emit_native_term.kai:372`, `KProj SInt64 → kaix_variant_arg_i64`), but it
**never fires**: `kaix_variant_arg_i64` is used **0 times** in *any* native
binary measured (both the lookup and insert IR dumps). The reason is one
line:

```
# stage2/compiler/kir_lower_bind.kai:86
pub fn bind_slot_kslot(st: LowerSt, cn: String, idx: Int) : KSlot = SBoxed
```

It hardcodes `SBoxed` for **every** match-arm field binder, kind-1 Int
included. The comment (lines 80–85) documents the state exactly:
construction/drop are i64-inline (#1083 layout) but "the read stays boxed —
correct, one box per read. The raw read (`SInt64` → `kaix_variant_arg_i64`)
is a **follow-up**." That follow-up has not landed on either backend front-
end. So suspect (a) and suspect (b) are the **same defect** measured from
two angles: the deferred kind-1 raw binder read *is* the boxed-shim cost on
the descent.

## 4. Suspect (b) — the kind-1 Int raw binder read: this IS the lever

The insert delta (1.99× → 2.79×) is the construction + RC path. But
construction is already raw (#1100 `kaix_variant_masked` builds from a raw
`i64` word buffer — verified, no box round-trip on the write side), and RC
is a **shared front-end cost**: BOTH backends emit `kai_incref` on the
borrowed children `l`/`r` per descent node (measured: C descent loop 3
incref-blocks, native 4 — near-identical), because Perceus does not elide
the dup on a read-only borrow. That RC residual is the #1104 Koka-parity
lever, tracked separately; it is not native-specific.

The **native-specific** excess on the full insert is the same boxed-read
binder as §3, now also paying it inside `insert_loop`, `balance_left`,
`balance_right`, and `is_red` — every match arm that reads a key/value/tag.
The native insert IR shows `kaix_variant_arg` ×204 + `kaix_tag_eq` ×12
(the latter a **boxed-Bool** tag comparison — `kaix_tag_eq` returns a heap-
tagged Bool, `emit_native_ops.kai:595`, where the C oracle compiles
`_scr->variant_tag == 14` to a raw `icmp`→`i1`→`condbr`). Same class of
representation divergence as the key read.

### The lever: land the kind-1 raw binder read (both front-end + native border)

Change `bind_slot_kslot` (`kir_lower_bind.kai:86`) to return `SInt64` when
`slot_kind_of(st, cn, idx) == 1` (the resolver at line 71 already exists and
is the one the construction path uses, so read/write cannot disagree). The
native emitter consumes it with no change (`emit_native_term.kai:372`
already routes `SInt64` → `kaix_variant_arg_i64`, a single raw load). The C
backend already reads raw, so the C oracle is the correctness reference.

**The blocker to respect (#709 phantom-box):** a raw `i64` binder that then
flows into a *boxed* consumer (a call arg, a boxed re-box, a slot store)
must re-box at that border, or a raw register reaches a `ptr`-typed consumer
and SIGSEGVs the native self-host. The construction side already solved the
symmetric border (`nemit_slot_word`, `emit_native_ops.kai:787`, re-boxes a
raw word for a boxed slot via `ptrtoint`). The read side needs the dual:
when a `SInt64` binder feeds a boxed consumer, insert a `kaix_int` re-box at
the border. This is why the naive #1102 attempt reverted — it read raw but
did not re-box at every boxed use.

### Expected magnitude (measured, instruction AND wall separated)

The §2b marginal attribution measured this directly: removing the native
boxed key-read saves ~0.44 G instructions per 20M reads but only ~0.045 G
cycles (marginal 0.10 cyc/insn — those instructions are near-free on the
pipeline). So the instruction win is large, the wall win is small:

- **Pure lookup**: removes the `kaix_variant_arg`+`kaix_int_field` pair per
  key read. Predicted native/C **insns 1.99× → ~1.2–1.4×**, but **cycles
  ~1.21× → ~1.15×** and **wall 9.04 s → ~8.6 s** (the cycle gap, not the
  instruction gap, is the wall). The README's historical C-backend figure
  for this change was 2.01× → 1.08× *in instructions*; native cannot beat
  C, and the wall move is IPC-diluted regardless.
- **Full insert**: the same read applies inside `insert_loop`/`balance_*`.
  Predicted native/C **insns 2.79× → ~2.0–2.2×**, but **wall 0.48 s →
  ~0.44–0.45 s** (recovering a proportional slice of native's 0.57 G excess
  cycles). It does NOT close the shared-front-end RC residual, so it cannot
  reach C's 0.35 s — and because native's IPC is already ~2.9, the wall
  payoff is well under the instruction payoff. **This is a real but modest
  wall lever, chosen because it is the cleanest single instruction reducer,
  not because it closes the gap.**

### How to measure it (before/after, fixed falsification)

1. **Instruction ratio (primary gate)**: `/usr/bin/time -l` on both the
   pure-lookup binary (rebuild from `rb_get` + a 20M-descent driver) and the
   insert binary, both backends. Gate: native/C pure-lookup insns must drop
   **below 1.6×** (from 1.99×). This is the host-dependent-but-reproducible
   number on this machine; the Docker-callgrind recipe applies to the C
   column only (native emits objects in-process, nothing for callgrind to
   `-g`-compile — `README.md` §instruction-count).
2. **`kaix_variant_arg_i64` fires**: `KAI_NATIVE_DUMP_IR` on the rb-tree
   build → `grep -c kaix_variant_arg_i64` must go from **0 to >0**, and the
   paired `kaix_variant_arg`+`kaix_int_field` count on the key slots must
   drop correspondingly. This is the mechanistic check that the lever
   actually engaged, independent of wall noise.
3. **Falsification (pin this before starting)**: if `kaix_variant_arg_i64`
   fires (check 2 passes) but the pure-lookup native/C insn ratio does NOT
   drop below ~1.7×, the model in §3 is wrong — the 2× is then NOT the
   boxed read binder but something downstream (the `ubfx` color-extraction
   on the tag header). Re-profile the descent-step histogram before touching
   anything else; do not proceed on momentum. (Note: the `kaix_tag_eq`
   boxed-Bool dispatch was measured in §2b as NOT a cost source — its native
   CPI is the lowest of any bench — so it is not the residual to suspect.)
   And because the wall payoff is IPC-diluted (§2b), gate the lane on the
   **cycle** delta too, not only the instruction delta: if instructions drop
   but cycles/wall do not move measurably, the lane is a no-op on the metric
   the user cares about and should not close claiming a wall win.
4. **Gates (this is an RC/representation change)**: selfhost byte-id
   (re-baseline expected — the emitted IR changes), backend serial parity
   (`BACKEND_PARITY_JOBS=1` — parallel is false-green per
   `native_preexisting_mac_parity_fails`), and the **native self-host gate**
   (a self-compile catches the #709 phantom-box SIGSEGV that macOS `-O0`
   hides; test at `-O2`), plus `KAI_TRACE_RC` balanced.

## 5. Honest uncertainty

- **The wall has no dramatic lever; the hardware absorbs the gap.** §2b
  measured native CPI *lower* than C in every bench (native is not
  stall-bound — no cache/branch/dependency regime to attack). Native's wall
  is purely its larger cycle count, and cycles = instructions ÷ IPC with an
  already-excellent IPC (2.9–3.8). So a 2.79× instruction gap is only a
  1.37× wall gap, and the one clean instruction reducer (kind-1 raw binder)
  buys ~0.03–0.05 s on the 1M insert — real, worth doing, but it will not
  bring native to C parity and the user who feels 0.48 s will feel ~0.44 s.
  If the goal is "native wall == C wall," **no single lever here delivers
  it** — that would need broad instruction reduction across the whole boxed
  representation (or the shared-front-end RC cut, which helps both backends
  and is the #1104 Koka-parity work). This is the truth over a lane that
  doesn't move the needle.
- **The boxed-Bool `kaix_tag_eq` dispatch is NOT a lever** (measured, §2b:
  `red_count` native CPI 0.26, the lowest of any bench). My first-pass
  falsification flagged it as a candidate; the measurement refutes it. A
  `kaix_tag_eq`→raw-`icmp` change would remove instructions but, like the
  key-read, at near-zero cycle cost — not worth a lane on wall grounds.
- **The kind-1 raw binder is still the recommended lane** — it is the
  cleanest, most-localised, correctness-gated instruction reducer, and it
  does move wall (modestly). But it is chosen as "the best available small
  win," not "the fix that closes the wall gap." Gate it on the **cycle**
  delta, not the instruction delta, so it is not closed claiming a wall win
  it did not produce.
- All numbers are this-machine (`37857e7d`, Darwin arm64, apple-silicon).
  CPI is host-microarchitecture-dependent (an in-order or narrower core
  would show a *higher* CPI and thus a *larger* wall payoff for the same
  instruction cut — apple-silicon's wide OoO core is the best case for
  "instructions are free"). Re-measure on the target host before quoting;
  the ratios are stable run-to-run here.

## Reproducing

```sh
# fresh native kaic2
touch stage2/main.kai && bash tools/gen-runtime-bc.sh && make -C stage2 KAI_LLVM=1 kaic2

# pure-lookup bench: rb_get over a fixed tree, 20M descents (build once)
#   (the driver used here: fill_loop then lookup_loop over rb_get, checksum-accumulated)
stage2/kaic2 --path stdlib --path <dir> lookup.kai > lookup.c && cc -O2 -I stage2 -I stage0 lookup.c -o lookup_c -lm
bin/kai build --backend=native lookup.kai -o lookup_native

# WALL + CPI: /usr/bin/time -l reports both `instructions retired` and
# `cycles elapsed` on this mac. CPI = cycles/insns is the wall discriminator
# (low CPI = cheap instructions, wall≈cycles; high CPI = stalls).
for b in lookup_c lookup_native; do
  /usr/bin/time -l ./$b >/dev/null 2>t
  ins=$(awk '/instructions retired/{print $1}' t); cyc=$(awk '/cycles elapsed/{print $1}' t)
  awk -v i=$ins -v c=$cyc 'BEGIN{printf "insns=%d cycles=%d CPI=%.3f\n",i,c,c/i}'
done

# mechanistic check: does the raw binder fire?
KAI_NATIVE_DUMP_IR=/tmp/x.ll bin/kai build --backend=native lookup.kai -o /dev/null
grep -c kaix_variant_arg_i64 /tmp/x.ll        # 0 today; >0 after the lever
```

Isolation benches used in §2b (all walk the same tree, differ by per-node
work): tag-walk = `rb_size` (tag dispatch, no key read); key-sum = a
`k0 + key_sum(l) + key_sum(r)` walk (reads the int key, no branch);
red-count = a `RBNode(Red,…)/RBNode(Black,…)` match (boxed-Bool color
dispatch, `kaix_tag_eq`+`kaix_truthy`). The key-read cost is `key-sum −
tag-walk`; the boxed-Bool cost is `red-count − tag-walk`.
