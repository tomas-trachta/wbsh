# 20 MB of random bytes, as when someone runs `cat` on a binary file.
# @stress size=120x40 maxms=20000 maxmem=300
param([int]$Scale = 1)
$rng = [System.Random]::new(7)
$bytes = [byte[]]::new(20MB * $Scale)
$rng.NextBytes($bytes)
,$bytes
