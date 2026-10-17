import Data.List (intercalate)

main :: IO ()
main = print (length (intercalate "," (map show [1 .. 300000 :: Int])))
