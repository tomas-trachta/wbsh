# REP, ICH, ECH and cursor moves with counts near 2^31 must clamp, not loop or allocate.
# @stress size=200x60 maxms=3000 maxmem=200
param([int]$Scale = 1)
$e = [char]27
$out = 'x'
foreach ($n in 2147483647, 2147483648, 4294967295, 99999999999999) {
	$out += "$e[${n}b$e[${n}@$e[${n}X$e[${n}C$e[${n}A$e[${n}L$e[${n}M$e[${n}P$e[${n}S$e[${n}T$e[${n};${n}r$e[${n}Ix"
}
$out * (20 * $Scale)
