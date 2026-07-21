# tls-refs.awk — classify every thread-local reference in an LLVM module.
#
#   awk -f tls-refs.awk module.ll module.ll      (the file is read TWICE:
#   attribute groups live at the end of the module and must be known before
#   the function bodies that cite them are classified)
#
# Emits one `<symbol> <function> <accessor|exposed>` triple per
# (thread-local, materialising function) pair.
#
#   accessor  the function is `noinline` and keeps the address to itself, so
#             the address is resolved inside a single activation — thread
#             identity is constant for its whole duration.
#   exposed   the function is inlinable, so the optimiser may fold the
#             address materialisation into a caller whose frame spans a
#             context switch; or the function hands the address back to its
#             caller, which has the same effect one frame up.
#
# `llvm.threadlocal.address` is `speculatable memory(none)`: LLVM is entitled
# to hoist, sink and CSE it. That is the whole reason the property has to be
# `noinline` on the materialising function rather than a rule about where the
# address is used.

NR == FNR {
    if ($0 ~ /^@[A-Za-z0-9_.$]+[[:space:]]*=/ && $0 ~ /thread_local/) {
        sym = $1; sub(/^@/, "", sym); tls[sym] = 1
    }
    if ($0 ~ /^attributes #[0-9]+ = /) {
        grp = $2; sub(/^#/, "", grp); noinline[grp] = ($0 ~ /[{ ]noinline[ }]/)
    }
    next
}

/^define / {
    fn = "<anonymous>"
    if (match($0, /@[A-Za-z0-9_.$]+\(/)) fn = substr($0, RSTART + 1, RLENGTH - 2)
    grp = ""
    tail = $0
    while (match(tail, /#[0-9]+/)) {
        grp = substr(tail, RSTART + 1, RLENGTH - 1)
        tail = substr(tail, RSTART + RLENGTH)
    }
    curfn = fn
    inlinable[fn] = !noinline[grp]
    delete taint
    next
}

/^}/ { curfn = ""; next }

curfn != "" {
    # Every mention of a thread-local inside a body materialises its address.
    rest = $0
    while (match(rest, /@[A-Za-z0-9_.$]+/)) {
        sym = substr(rest, RSTART + 1, RLENGTH - 1)
        if (sym in tls) ref[sym, curfn] = 1
        rest = substr(rest, RSTART + RLENGTH)
    }

    # Taint the SSA values that ARE the address (never the values loaded
    # THROUGH it: a load is re-done per activation, an address is not).
    lhs = ""
    if (match($0, /^[[:space:]]*%[-A-Za-z0-9_.]+ = /)) {
        lhs = substr($0, RSTART, RLENGTH); gsub(/[[:space:]=]/, "", lhs)
    }
    if (lhs != "") {
        if ($0 ~ /@llvm\.threadlocal\.address/) taint[lhs] = 1
        else if ($0 ~ /= (tail |musttail |notail )*(getelementptr|bitcast|addrspacecast|phi|select|freeze)[ (]/ &&
                 operand_tainted($0)) taint[lhs] = 1
    }

    # Returning the address defeats the accessor: the caller now holds it and
    # can keep holding it across a switch.
    if ($0 ~ /^[[:space:]]*ret / && operand_tainted($0)) leaks[curfn] = 1
}

# The instruction's own result never needs excluding: SSA assigns each name
# once, so a line's LHS cannot already be tainted when the line is read.
function operand_tainted(line,   scan, name) {
    scan = line
    while (match(scan, /%[-A-Za-z0-9_.]+/)) {
        name = substr(scan, RSTART, RLENGTH)
        if (name in taint) return 1
        scan = substr(scan, RSTART + RLENGTH)
    }
    return 0
}

END {
    for (key in ref) {
        split(key, part, SUBSEP)
        sym = part[1]; fn = part[2]
        print sym, fn, ((inlinable[fn] || leaks[fn]) ? "exposed" : "accessor")
    }
}
