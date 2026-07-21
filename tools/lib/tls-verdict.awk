# tls-verdict.awk — the thread-local hoist policy: an allow-list against what
# the bitcode actually says.
#
#   awk -f tls-verdict.awk <allow-list> <sorted-triples>
#   awk -v report=1 -f tls-verdict.awk /dev/null <sorted-triples>
#
# First file: `<symbol> <accessor|exposed>`, `#` comments and blanks ignored.
# Second: the `<symbol> <function> <accessor|exposed>` triples tls-refs.awk
# emits, SORTED (so a symbol's references arrive together and the output is
# deterministic). A symbol is accessor-only when EVERY function materialising
# its address is.
#
# Diagnoses go to stderr, the summary to stdout. Exit 1 on any violation, 2 on
# a malformed allow-list. Under `report=1` it prints the folded classification
# and never fails.

function err(m)  { print "tls-hoist-gate: " m > "/dev/stderr" }
function hint(m) { print "  " m > "/dev/stderr" }

# Keyed on the filename, not `FNR == NR`: the allow-list may legitimately be
# empty (/dev/null in report mode), and an empty first file makes that idiom
# misread the second file as the first.
FILENAME == ARGV[1] {
    sub(/#.*/, "")
    if (NF == 0) next
    if (NF != 2 || ($2 != "accessor" && $2 != "exposed")) {
        err("malformed allow-list line: " $0); fatal = 2; next
    }
    want[$1] = $2
    order[++nwant] = $1
    next
}

fatal { next }        # a malformed policy makes every comparison meaningless

$1 != cur { flush(); cur = $1; every = ""; offenders = "" }

{ every = every " " $2; if ($3 == "exposed") offenders = offenders " " $2 }

END {
    if (fatal) exit fatal
    flush()
    if (report) exit 0
    for (i = 1; i <= nwant; i++) {
        if (order[i] in seen) continue
        err("STALE allow-list entry '" order[i] "' (not referenced by the hot bitcode)")
        rc = 1
    }
    print "tls-hoist-gate: thread-locals=" (n_acc + n_exp) " accessor=" n_acc " exposed=" n_exp
    exit rc
}

function flush(   got, fns) {
    if (cur == "") return
    got = (offenders != "" ? "exposed" : "accessor")
    fns = (offenders != "" ? offenders : every)
    if (got == "accessor") n_acc++; else n_exp++
    seen[cur] = 1
    if (report)          { print cur, got fns; return }
    if (want[cur] == got) return
    rc = 1
    if (want[cur] == "accessor") {
        err("REGRESSION — '" cur "' is listed accessor but is materialised by an inlinable function:" fns)
        hint("→ restore `__attribute__((noinline))` on it, or route the access through an accessor that keeps the address")
    } else if (want[cur] == "exposed") {
        err("RATCHET — '" cur "' is listed exposed but is now materialised only through noinline accessors")
        hint("→ promote it to `accessor` in tools/tls-hoist.allow")
    } else {
        err("UNLISTED thread-local '" cur "' in the hot bitcode, materialised by:" fns)
        hint("→ the sound fix is a `noinline` accessor that keeps the address, or keeping the symbol out")
        hint("  of the hot (KAI_HOT_ONLY) bitcode. Add `" cur " " got "` to tools/tls-hoist.allow only to")
        hint("  record the exposure as debt — read the class note there first.")
    }
}
