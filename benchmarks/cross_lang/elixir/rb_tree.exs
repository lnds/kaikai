# Functional red-black tree port for cross-language benchmark.
# Mirrors examples/perceus/rb_tree.kai. Pure functional rebuild on
# each insert; Erlang/BEAM's per-process heap mirrors the kaikai
# fiber-private heap discipline.

defmodule RBTree do
  @red 0
  @black 1

  def balance(@black, {@red, ll = {@red, _, _, _, _}, lk, lv, lr}, k, v, r) do
    {@red,
     {@black, elem(ll, 1), elem(ll, 2), elem(ll, 3), elem(ll, 4)},
     lk, lv,
     {@black, lr, k, v, r}}
  end

  def balance(@black, {@red, ll, lk, lv, lr = {@red, _, _, _, _}}, k, v, r) do
    {@red,
     {@black, ll, lk, lv, elem(lr, 1)},
     elem(lr, 2), elem(lr, 3),
     {@black, elem(lr, 4), k, v, r}}
  end

  def balance(@black, l, k, v, {@red, rl = {@red, _, _, _, _}, rk, rv, rr}) do
    {@red,
     {@black, l, k, v, elem(rl, 1)},
     elem(rl, 2), elem(rl, 3),
     {@black, elem(rl, 4), rk, rv, rr}}
  end

  def balance(@black, l, k, v, {@red, rl, rk, rv, rr = {@red, _, _, _, _}}) do
    {@red,
     {@black, l, k, v, rl},
     rk, rv,
     {@black, elem(rr, 1), elem(rr, 2), elem(rr, 3), elem(rr, 4)}}
  end

  def balance(c, l, k, v, r), do: {c, l, k, v, r}

  def insert_loop(nil, k, v), do: {@red, nil, k, v, nil}
  def insert_loop({c, l, k0, v0, r}, k, v) do
    cond do
      k < k0 -> balance(c, insert_loop(l, k, v), k0, v0, r)
      k > k0 -> balance(c, l, k0, v0, insert_loop(r, k, v))
      true   -> {c, l, k0, v, r}
    end
  end

  def rb_insert(t, k, v) do
    case insert_loop(t, k, v) do
      nil -> nil
      {_, l, k0, v0, r} -> {@black, l, k0, v0, r}
    end
  end

  def rb_size(nil), do: 0
  def rb_size({_, l, _, _, r}), do: 1 + rb_size(l) + rb_size(r)

  def rb_height(nil), do: 0
  def rb_height({_, l, _, _, r}) do
    lh = rb_height(l)
    rh = rb_height(r)
    1 + max(lh, rh)
  end

  def lcg_next(s), do: rem(s * 1664525 + 1013904223, 2147483647)

  def fill(t, _seed, 0), do: t
  def fill(t, seed, i) do
    s = lcg_next(seed)
    fill(rb_insert(t, s, i), s, i - 1)
  end

  def run() do
    n = 1_000_000
    start = System.monotonic_time(:nanosecond)
    root = fill(nil, 1, n)
    elapsed_ns = System.monotonic_time(:nanosecond) - start

    secs = div(elapsed_ns, 1_000_000_000)
    ms = rem(div(elapsed_ns, 1_000_000), 1000)
    ms_str = String.pad_leading(Integer.to_string(ms), 3, "0")

    IO.puts("elixir (functional RB) rb-tree benchmark")
    IO.puts("inserts: #{n}")
    IO.puts("size: #{rb_size(root)}")
    IO.puts("height: #{rb_height(root)}")
    IO.puts("elapsed: #{secs}.#{ms_str}s")
  end
end

RBTree.run()
