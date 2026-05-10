defmodule Bench do
  def fib(n) when n < 2, do: n
  def fib(n), do: fib(n - 1) + fib(n - 2)
end

IO.puts(Bench.fib(35))
