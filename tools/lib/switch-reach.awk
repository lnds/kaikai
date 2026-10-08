# switch-reach.awk — the functions of an LLVM module that can switch context.
#
#   awk -f switch-reach.awk module.ll module.ll      (read twice: attribute
#   groups sit at the end of the module and decide which calls return)
#
# Prints `<function> <callee>` per line: every defined function whose
# activation can continue after a context switch, and the call that lets it
# (`*` for a call through a pointer). A function switches when it calls
#   - a switch primitive (swapcontext, setcontext, kai_seg_switch),
#   - a `returns_twice` function (setjmp: a longjmp can re-enter the frame
#     from another stack),
#   - through a pointer (it may run kaikai code, which can park),
#   - an undefined `kai*` symbol (program code linked beside the owner),
#   - or a defined function that switches.
# A call that never returns (`noreturn`) does not count: nothing after it runs
# in this activation. Undefined functions outside the `kai` namespace are libc
# and never switch a fiber.

function primitive(name) {
    return name == "swapcontext" || name == "setcontext" || name == "kai_seg_switch"
}

function groups_of(line, out,   tail, n) {
    n = 0
    tail = line
    while (match(tail, /#[0-9]+/)) {
        out[++n] = substr(tail, RSTART + 1, RLENGTH - 1)
        tail = substr(tail, RSTART + RLENGTH)
    }
    return n
}

function has_attr(line, attr,   g, n, i) {
    if (line ~ ("[ )]" attr "( |$)")) return 1
    n = groups_of(line, g)
    for (i = 1; i <= n; i++) if ((g[i], attr) in grp_has) return 1
    return 0
}

NR == FNR {
    if ($0 ~ /^attributes #[0-9]+ = /) {
        id = $2; sub(/^#/, "", id)
        if ($0 ~ /[{ ]noreturn[ }]/)      grp_has[id, "noreturn"] = 1
        if ($0 ~ /[{ ]returns_twice[ }]/) grp_has[id, "returns_twice"] = 1
    }
    next
}

/^(define|declare) / {
    name = ""
    if (match($0, /@[-A-Za-z0-9_.$]+\(/)) name = substr($0, RSTART + 1, RLENGTH - 2)
    if (has_attr($0, "noreturn"))      noreturn[name] = 1
    if (has_attr($0, "returns_twice")) twice[name] = 1
    if ($0 ~ /^define /) { defined[name] = 1; curfn = name; nfn++; fns[nfn] = name }
    next
}

/^}/ { curfn = ""; next }

curfn != "" && /(^|[ =])(tail |musttail |notail )?(call|invoke) / {
    line = $0
    if (line ~ /(call|invoke) [^@%]* asm /) next
    if (match(line, /(call|invoke) [^@]*@("[^"]*"|[-A-Za-z0-9_.$]+)\(/)) {
        callee = substr(line, RSTART, RLENGTH)
        sub(/^[^@]*@/, "", callee); sub(/\($/, "", callee); gsub(/"/, "", callee)
        if (callee ~ /^llvm\./) next
        if (has_attr(line, "noreturn")) next
        edges[curfn, ++nedge[curfn]] = callee
    } else {
        indirect[curfn] = 1
    }
}

END {
    for (i = 1; i <= nfn; i++) {
        fn = fns[i]
        if (indirect[fn]) sw[fn] = "*"
        for (j = 1; j <= nedge[fn]; j++) {
            c = edges[fn, j]
            if (c in noreturn) continue
            if (primitive(c) || (c in twice) || (!(c in defined) && c ~ /^_?kai/)) sw[fn] = c
        }
    }
    do {
        changed = 0
        for (i = 1; i <= nfn; i++) {
            fn = fns[i]
            if (fn in sw) continue
            for (j = 1; j <= nedge[fn]; j++) {
                c = edges[fn, j]
                if ((c in sw) && !(c in noreturn)) { sw[fn] = c; changed = 1; break }
            }
        }
    } while (changed)
    for (fn in sw) print fn, sw[fn]
}
