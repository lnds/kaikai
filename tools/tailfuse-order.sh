#!/bin/sh
# usage: tailfuse-order.sh <file.c>
# One line per fused-group member in the emitted C: the member, the fused
# function its wrapper forwards to, and the tag it passes.
awk '/^KAI_CONST static [a-z0-9_ *]+ kaiu_[A-Za-z0-9_]+\(/ { match($0, /kaiu_[A-Za-z0-9_]+\(/); fn = substr($0, RSTART + 5, RLENGTH - 6); next }
     fn != "" && /return kaiu___kai_fused_[A-Za-z0-9_]+\([0-9]+LL/ { match($0, /kaiu___kai_fused_[A-Za-z0-9_]+\([0-9]+/); s = substr($0, RSTART + 5, RLENGTH - 5); split(s, a, "("); print fn, a[1], a[2] }
     { fn = "" }' "$1"
