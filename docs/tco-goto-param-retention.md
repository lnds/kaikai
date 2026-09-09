# TCO goto param retention — the borrowed-arg drop

A parameter of a self-tail-recursive function leaked one reference per
iteration when its read was passed to a callee in the self-call's own
arguments. The `goto` rebinds the parameter slot without dropping the value
it held. Shapes live in `examples/perceus/tco_*_1902.kai`, gated by
`make -C stage2 test-tco-borrow-drop-1902`.

## The class, and where the line falls

The distinction the drop decision needs is **does the callee release the
ref?** — not "does it store the value?". Two callees that store nothing
require opposite decisions:

```c
/* discards e: emits no decref  →  the goto owes the drop, else it leaks */
static KaiValue *kaiu_fresh(KaiValue *kaiv_e, KaiValue *kaiv_p) {
    return kai_record(1, (KaiValue *[]){kai_cons(kaiv_p, kai_nil())}, ...);
}

/* reads e.entries: emits kai_decref(_b)  →  a goto drop DOUBLE-FREES */
static KaiValue *kaiu_env_prepend(KaiValue *kaiv_e, KaiValue *kaiv_p) {
    return kai_record(1, (KaiValue *[]){kai_cons(kaiv_p, kai_incref(({
        KaiValue *_b = kaiv_e; KaiValue *_f = kai_op_field_at(_b, 0);
        kai_decref(_b); _f; })))}, ...);
}
```

`ConsumeIndex` classifies both as non-consuming, so the consume map cannot
separate them. The **borrow map** can, and already did: a borrowed parameter
slot is exactly the slot whose callee takes no ownership. The mechanism was
built and reachable — `pcs_build_borrow_map` registers `fresh` at position 0,
and a non-TCO caller already skips the dup and keeps its own drop — but the
TCO pre-pass never consulted it.

## What the fix wires

Three seams, all of them threading the existing borrow map to a decision that
was making the call without it:

- `pcs_consumed_b_expr` — a read landing in a borrowed slot no longer
  witnesses consumption. It cannot: the caller still holds a ref the callee
  never released.
- `pcs_branch_aware_skip_params_b` — the skip-set built on that witness.
- `tcrec_rule3_mask_b` — the per-site rule. When every read of a parameter in
  a self-call's args is a bare read in a borrowed slot, and rules 1+2 did not
  already claim the bit, the goto takes the drop.

The per-site placement is what makes it sound. A drop hoisted before the
arguments would be release-then-read (`pcs_drops_dead_in` rejects exactly
that); the goto's dropmask fires after `arg_lets` and before the rebinds,
which is the only point where the old value is dead and the slot still holds
it.

## Measured

| fixture | leaked before | leaked after | live_peak after | runs |
|---|---:|---:|---:|---|
| `tco_discarded_param_1902` | 599,998 | **0** | 6 | `n=1` |
| `tco_discarded_param_variant_1902` | 599,998 | **1** | 7 | `n=1` |
| `tco_field_read_param_1902` | 399,998 | 399,998 | 400,003 | `n=200000` |
| `tco_field_read_via_let_1902` | 399,998 | 399,998 | 400,003 | `n=200000` |
| `tco_plain_list_control_1902` | 0 | 0 | 400,001 | `n=200000` |
| `tco_non_tail_control_1902` | 3 | 3 | 8 | `n=20000` |
| `tco_param_rethreaded_control_1902` | 0 | 0 | 5 | `n=0` |

Identical on both backends. The field-read rows are unchanged **by design**:
their callee already emits the receiver decref, so the goto must not drop —
and their retention belongs to a different class (the `kai_incref` on the
field the record keeps), not to this one.

Self-compile (C backend, `KAI_TRACE_RC=1`, `stage2/main.kai`):
`leaked` 192,523,253 → 192,010,270 (**−512,983, −0.27%**);
`alloc_total` 325,595,230 → 325,075,799.

That number is the honest scope of the class: real, closed, and a small
fraction of the compiler's retention. The bulk lives elsewhere.

## Traps for whoever measures next

**A leak count is not a correctness check.** An earlier attempt gated on the
skip-set alone, took the discarding shapes to `leaked=0` with correct output,
and left a use-after-free in the field-read shapes — which crashed *after*
the counters printed. Every shape claimed fixed must RUN to completion and
print correctly; the gate asserts both halves, and the field-read fixtures are
run for exit status precisely because their failure mode is a double free.

**`KAI_TRACE_RC_LEAKSITE` needs both flags.** Building with
`-DKAI_TRACE_RC_LEAKSITE=1` alone fails to compile: the definitions sit inside
an enclosing `#ifdef KAI_TRACE_RC`. Pass `-DKAI_TRACE_RC=1
-DKAI_TRACE_RC_LEAKSITE=1`.
