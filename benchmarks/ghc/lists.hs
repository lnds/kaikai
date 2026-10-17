import Data.List (foldl')

main :: IO ()
main = do
  let xs = [1 .. 3000000] :: [Int]
      ys = filter even (map (* 3) xs)
  print (foldl' (+) 0 ys)
