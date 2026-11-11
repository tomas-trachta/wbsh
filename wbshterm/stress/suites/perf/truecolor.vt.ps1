# 24-bit colour on every cell, foreground and background (gradients, bat, delta).
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 4000 * $Scale; $i++) {
	for ($c = 0; $c -lt 100; $c++) { [void]$sb.Append("$e[38;2;$($c * 2);$($i % 256);128;48;2;0;$($c * 2);$($i % 256)m#") }
	[void]$sb.Append("$e[0m`r`n")
}
$sb.ToString()
