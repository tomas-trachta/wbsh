# Every private mode on and off many times, interleaved with text.
# @stress size=80x24 maxms=10000 maxmem=200
param([int]$Scale = 1)
$e = [char]27
$modes = 1, 3, 5, 6, 7, 12, 25, 47, 66, 1000, 1002, 1003, 1004, 1005, 1006, 1007, 1015, 1047, 1048, 1049, 2004, 2026, 9001
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 2000 * $Scale; $i++) {
	foreach ($m in $modes) { [void]$sb.Append("$e[?${m}h") }
	[void]$sb.Append("text $i")
	foreach ($m in $modes) { [void]$sb.Append("$e[?${m}l") }
}
$sb.ToString()
