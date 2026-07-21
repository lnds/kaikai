; Self-test fixture for tools/lib/tls-refs.awk, exercising the three shapes the
; classifier has to tell apart. Hand-written rather than compiled: the gate must
; be able to prove it discriminates on a host with no clang 18, where no runtime
; bitcode exists at all.

@st_safe = external thread_local(initialexec) global i64, align 8
@st_hoistable = external thread_local(initialexec) global i64, align 8
@st_leaked = external thread_local(initialexec) global i64, align 8

declare ptr @llvm.threadlocal.address.p0(ptr)

; noinline, and the address never leaves the frame — the accessor shape.
define i64 @st_read_safe() #0 {
  %1 = call align 8 ptr @llvm.threadlocal.address.p0(ptr align 8 @st_safe)
  %2 = load i64, ptr %1, align 8
  ret i64 %2
}

; Same body, inlinable: the optimiser may fold the address materialisation into
; a caller whose frame spans a context switch.
define i64 @st_read_hoistable() #1 {
  %1 = call align 8 ptr @llvm.threadlocal.address.p0(ptr align 8 @st_hoistable)
  %2 = load i64, ptr %1, align 8
  ret i64 %2
}

; noinline, but hands the address back: the caller now holds it, so the hoist
; happens one frame up instead.
define ptr @st_addr_of_leaked() #0 {
  %1 = call align 8 ptr @llvm.threadlocal.address.p0(ptr align 8 @st_leaked)
  %2 = getelementptr inbounds i8, ptr %1, i64 0
  ret ptr %2
}

attributes #0 = { noinline nounwind }
attributes #1 = { nounwind }
