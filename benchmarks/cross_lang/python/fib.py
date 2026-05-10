import sys

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

sys.setrecursionlimit(10000)
print(fib(35))
