; Hand-written IR for `tls-hoist-gate.sh --self-test`, owner half. Each
; function is one shape the owner gate must classify; the expected verdicts
; are listed in the script.

@kai_worker = thread_local global i64 0
@st_tls = thread_local global i64 0

declare i32 @swapcontext(ptr, ptr)
declare void @st_libc()
declare void @kai_program_fn()
declare void @st_exit() #1
declare ptr @llvm.threadlocal.address.p0(ptr)

; The scheduler accessor: noinline, may return the worker's address.
define ptr @kai_worker_here() #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @kai_worker)
  ret ptr %a
}

; Leaf accessor: reads a thread-local, calls only libc. Passes.
define i64 @st_leaf() #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  %v = load i64, ptr %a
  call void @st_libc()
  ret i64 %v
}

; Switches directly. Fails.
define void @st_switches(ptr %c) #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  %r = call i32 @swapcontext(ptr %c, ptr %c)
  store i64 1, ptr %a
  ret void
}

define void @st_helper(ptr %c) #0 {
  %r = call i32 @swapcontext(ptr %c, ptr %c)
  ret void
}

; Switches through a defined callee. Fails.
define void @st_reaches(ptr %c) #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  call void @st_helper(ptr %c)
  store i64 1, ptr %a
  ret void
}

; Calls through a pointer, which may run kaikai code. Fails.
define void @st_indirect(ptr %f) #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  call void %f()
  store i64 1, ptr %a
  ret void
}

; Calls program code linked beside the owner. Fails.
define void @st_program() #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  call void @kai_program_fn()
  store i64 1, ptr %a
  ret void
}

define void @st_dies(ptr %c) #1 {
  %r = call i32 @swapcontext(ptr %c, ptr %c)
  call void @st_exit()
  unreachable
}

; Its only switching callee never returns. Passes.
define void @st_noreturn(ptr %c) #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  store i64 1, ptr %a
  call void @st_dies(ptr %c)
  unreachable
}

; Hands a thread-local address to its caller. Fails.
define ptr @st_leaks() #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @st_tls)
  ret ptr %a
}

; Reads the scheduler's thread-local outside its accessor. Fails.
define i64 @st_sched_direct() #0 {
  %a = call ptr @llvm.threadlocal.address.p0(ptr @kai_worker)
  %v = load i64, ptr %a
  ret i64 %v
}

attributes #0 = { noinline nounwind }
attributes #1 = { noinline noreturn nounwind }
