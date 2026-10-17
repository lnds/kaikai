data Color = R | B
data Tree = E | T !Color Tree !Int !Int Tree

balance :: Color -> Tree -> Int -> Int -> Tree -> Tree
balance B (T R (T R a xk xv b) yk yv c) zk zv d = T R (T B a xk xv b) yk yv (T B c zk zv d)
balance B (T R a xk xv (T R b yk yv c)) zk zv d = T R (T B a xk xv b) yk yv (T B c zk zv d)
balance B a xk xv (T R (T R b yk yv c) zk zv d) = T R (T B a xk xv b) yk yv (T B c zk zv d)
balance B a xk xv (T R b yk yv (T R c zk zv d)) = T R (T B a xk xv b) yk yv (T B c zk zv d)
balance c l k v r = T c l k v r

insert :: Int -> Int -> Tree -> Tree
insert k v t = blacken (ins t)
  where
    ins E = T R E k v E
    ins (T c l k' v' r)
      | k < k' = balance c (ins l) k' v' r
      | k > k' = balance c l k' v' (ins r)
      | otherwise = T c l k v r
    blacken (T _ l k' v' r) = T B l k' v' r
    blacken E = E

size :: Tree -> Int
size E = 0
size (T _ l _ _ r) = size l + 1 + size r

lcg :: Int -> Int
lcg s = (s * 1664525 + 1013904223) `mod` 2147483647

fill :: Tree -> Int -> Int -> Tree
fill t _ 0 = t
fill t s n = let s' = lcg s in t `seq` fill (insert s' n t) s' (n - 1)

main :: IO ()
main = print (size (fill E 42 1000000))
