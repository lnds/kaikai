# Functional-style red-black tree port for cross-language benchmark.
# Mirrors examples/perceus/rb_tree.kai. Arrays act as nodes:
# [color, left, key, value, right] or nil for the leaf.

RED = 0
BLACK = 1

def balance(c, l, k, v, r)
  if c == BLACK
    if l && l[0] == RED
      ll = l[1]
      if ll && ll[0] == RED
        return [RED,
                [BLACK, ll[1], ll[2], ll[3], ll[4]],
                l[2], l[3],
                [BLACK, l[4], k, v, r]]
      end
      lr = l[4]
      if lr && lr[0] == RED
        return [RED,
                [BLACK, l[1], l[2], l[3], lr[1]],
                lr[2], lr[3],
                [BLACK, lr[4], k, v, r]]
      end
    end
    if r && r[0] == RED
      rl = r[1]
      if rl && rl[0] == RED
        return [RED,
                [BLACK, l, k, v, rl[1]],
                rl[2], rl[3],
                [BLACK, rl[4], r[2], r[3], r[4]]]
      end
      rr = r[4]
      if rr && rr[0] == RED
        return [RED,
                [BLACK, l, k, v, r[1]],
                r[2], r[3],
                [BLACK, rr[1], rr[2], rr[3], rr[4]]]
      end
    end
  end
  [c, l, k, v, r]
end

def insert_loop(t, k, v)
  return [RED, nil, k, v, nil] if t.nil?
  c, l, k0, v0, r = t
  if k < k0
    balance(c, insert_loop(l, k, v), k0, v0, r)
  elsif k > k0
    balance(c, l, k0, v0, insert_loop(r, k, v))
  else
    [c, l, k0, v, r]
  end
end

def rb_insert(t, k, v)
  r = insert_loop(t, k, v)
  return nil if r.nil?
  [BLACK, r[1], r[2], r[3], r[4]]
end

def rb_size(t)
  return 0 if t.nil?
  1 + rb_size(t[1]) + rb_size(t[4])
end

def rb_height(t)
  return 0 if t.nil?
  l = rb_height(t[1])
  r = rb_height(t[4])
  1 + (l > r ? l : r)
end

def lcg_next(s)
  (s * 1664525 + 1013904223) % 2147483647
end

root = nil
seed = 1
n = 1_000_000

start = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond)
i = n
while i > 0
  seed = lcg_next(seed)
  root = rb_insert(root, seed, i)
  i -= 1
end
elapsed_ns = Process.clock_gettime(Process::CLOCK_MONOTONIC, :nanosecond) - start

secs = elapsed_ns / 1_000_000_000
ms = (elapsed_ns / 1_000_000) % 1000

puts "ruby (functional RB) rb-tree benchmark"
puts "inserts: #{n}"
puts "size: #{rb_size(root)}"
puts "height: #{rb_height(root)}"
puts "elapsed: #{secs}.#{ms.to_s.rjust(3, '0')}s"
