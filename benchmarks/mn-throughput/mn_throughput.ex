defmodule MnThroughput do
  @moduledoc """
  BEAM side of `benchmarks/mn-throughput` — the same shape as
  `mn_throughput.kai`: `workers` processes each grind a pure
  tail-recursive sum and send the partial to the collector.

  `grind/2` mirrors the kaikai function instruction for instruction
  (`rem(i, 7)` is kaikai's `i % 7` for positive `i`), so both sides do
  the same arithmetic the same number of times and print the same total.
  """

  @doc "Tail-recursive partial sum of `rem(i, 7)` for i in load..1."
  def grind(i, acc) when i <= 0, do: acc
  def grind(i, acc), do: grind(i - 1, acc + rem(i, 7))

  @doc "Fan out `workers` processes over `load` iterations each; sum the partials."
  def run(workers, load) do
    parent = self()
    Enum.each(1..workers, fn _ -> spawn(fn -> send(parent, {:partial, grind(load, 0)}) end) end)
    collect(workers, 0)
  end

  @doc "Entry point for `elixir -e`: times `run/2` and prints the harness line."
  def main(workers, load) do
    {elapsed_ns, total} = :timer.tc(fn -> run(workers, load) end, :nanosecond)
    IO.puts("workers=#{workers} load=#{load} total=#{total} elapsed_ns=#{elapsed_ns}")
  end

  defp collect(0, acc), do: acc

  defp collect(remaining, acc) do
    receive do
      {:partial, partial} -> collect(remaining - 1, acc + partial)
    end
  end
end
