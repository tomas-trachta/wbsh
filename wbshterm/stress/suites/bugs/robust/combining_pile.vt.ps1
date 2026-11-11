# One base character with a million combining marks (zalgo text).
# @stress size=80x24 maxms=5000 maxmem=200
param([int]$Scale = 1)
"a" + ([string][char]0x301 * (1000000 * $Scale)) + "b"
