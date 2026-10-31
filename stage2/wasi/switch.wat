;; Context switch for wasm32-wasi: the `kai` imports of the runtime's wasm_ctx.h,
;; implemented with the stack-switching proposal. Merged into a fiber program by
;; `kai build`; `_start` here runs the program's own `_start` as the root context
;; and dispatches every switch.
;;
;; A context is a continuation. The running one suspends with the target it wants
;; and the dispatcher below resumes (or starts) that target, so exactly one
;; context runs at a time.
(module
  (type $ft (func))
  (type $kt (cont $ft))
  (type $enter (func (param i32)))
  (import "main" "_start" (func $main_start))
  (import "main" "__indirect_function_table" (table $fns 0 funcref))
  (import "main" "__stack_pointer" (global $sp (mut i32)))

  (tag $park (param i32 i32 i32 i32))
  (tag $exit (param i32 i32 i32))

  (table $conts 64 (ref null $kt))
  (global $boot_fn (mut i32) (i32.const 0))
  (global $boot_id (mut i32) (i32.const 0))
  (elem declare func $boot $root)

  (func $boot (type $ft)
    (call_indirect $fns (type $enter) (global.get $boot_id) (global.get $boot_fn)))

  (func $root (type $ft)
    (call $main_start))

  (func (export "needs_suspension"))

  (func (export "ctx_switch") (param $from i32) (param $to i32) (param $stack i32) (param $fn i32)
    (suspend $park (local.get $from) (local.get $to) (local.get $stack) (local.get $fn)))

  (func (export "ctx_exit") (param $to i32) (param $stack i32) (param $fn i32)
    (suspend $exit (local.get $to) (local.get $stack) (local.get $fn))
    (unreachable))

  (func $save (param $id i32) (param $k (ref $kt))
    (if (i32.ge_u (local.get $id) (table.size $conts))
      (then
        (drop (table.grow $conts (ref.null $kt)
          (i32.add (i32.sub (local.get $id) (table.size $conts)) (i32.const 64))))))
    (table.set $conts (local.get $id) (local.get $k)))

  (func $pick (param $to i32) (param $stack i32) (param $fn i32) (result (ref $kt))
    (local $k (ref null $kt))
    (if (i32.lt_u (local.get $to) (table.size $conts))
      (then
        (local.set $k (table.get $conts (local.get $to)))
        (table.set $conts (local.get $to) (ref.null $kt))))
    (if (ref.is_null (local.get $k))
      (then
        (global.set $sp (local.get $stack))
        (global.set $boot_fn (local.get $fn))
        (global.set $boot_id (local.get $to))
        (local.set $k (cont.new $kt (ref.func $boot)))))
    (ref.as_non_null (local.get $k)))

  (func (export "_start")
    (local $k (ref null $kt))
    (local $from i32) (local $to i32) (local $stack i32) (local $fn i32)
    (local.set $k (cont.new $kt (ref.func $root)))
    (loop $next
      (block $done
        (block $on_exit (result i32 i32 i32 (ref $kt))
          (block $on_park (result i32 i32 i32 i32 (ref $kt))
            (resume $kt (on $park $on_park) (on $exit $on_exit) (ref.as_non_null (local.get $k)))
            (br $done))
          (local.set $k)
          (local.set $fn)
          (local.set $stack)
          (local.set $to)
          (local.set $from)
          (call $save (local.get $from) (ref.as_non_null (local.get $k)))
          (local.set $k (call $pick (local.get $to) (local.get $stack) (local.get $fn)))
          (br $next))
        (drop)
        (local.set $fn)
        (local.set $stack)
        (local.set $to)
        (local.set $k (call $pick (local.get $to) (local.get $stack) (local.get $fn)))
        (br $next)))))
