# tls-owner-verdict.awk — the thread-local policy for the runtime owner.
#
#   awk -v sched=<sym> -v accessor=<fn> -f tls-owner-verdict.awk \
#       <allow-list> <switching functions> <detail triples>
#
# First file: `<function> pinned` lines (tools/tls-owner.allow).
# Second: `<function> <callee>` lines, the functions switch-reach.awk found
# able to switch context and the call through which they do.
# Third: `<symbol> <function> <inlinable> <leaks>` lines from
# `tls-refs.awk -v detail=1`.
#
# The owner is optimised, so a thread-local address resolved in a function
# can be kept for the whole activation. That is sound only while the
# activation cannot outlive a switch. A reference fails when:
#   - its function can switch context, unless the allow-list pins it to
#     one thread;
#   - its function hands the address back to a caller;
#   - it names the scheduler's thread-local outside its accessor (the
#     accessor alone may return that address: holding it across a switch is
#     the caller's bug, which the formal model rules out at the source).
# Exit 1 on any failure, on an allow-list entry the module no longer needs,
# or when the accessor no longer reads the scheduler thread-local at all.
# Several modules can be checked in one run by concatenating their lines; an
# entry is stale only when no module needs it.

function err(m) { print "tls-owner-gate: " m > "/dev/stderr"; rc = 1 }

FILENAME == ARGV[1] {
    sub(/#.*/, "")
    if (NF == 0) next
    if (NF != 2 || $2 != "pinned") { err("malformed allow-list line: " $0); next }
    pinned[$1] = 1
    next
}

FILENAME == ARGV[2] { sw[$1] = $2; next }

{
    sym = $1; fn = $2; leaks = $4
    n++
    if (sym == sched) {
        if (fn == accessor) { seen_accessor = 1; if (fn in sw) err(accessor " can switch context") }
        else err("scheduler thread-local '" sym "' read in " fn "; only " accessor " may read it")
        next
    }
    if (leaks) err("'" sym "' leaves " fn " as an address its caller can keep across a switch")
    else if (fn in sw) {
        if (fn in pinned) used[fn] = 1
        else err("'" sym "' is materialised in " fn ", which can switch context (through " sw[fn] ")")
    }
}

END {
    for (fn in pinned) if (!(fn in used)) err("STALE allow-list entry '" fn "' (it no longer keeps a thread-local across a switch)")
    if (!seen_accessor) err("the scheduler thread-local '" sched "' is not read by " accessor)
    print "tls-owner-gate: references=" n (rc ? " FAIL" : " ok")
    exit rc
}
