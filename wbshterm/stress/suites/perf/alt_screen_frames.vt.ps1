# Whole-screen repaints on the alternate screen, like htop or a TUI at 60 fps.
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$row = ('cpu ' + ('|' * 50) + ' 42.0%  ') * 2
$sb = [System.Text.StringBuilder]::new()
for ($f = 0; $f -lt 400 * $Scale; $f++) {
	if ($f % 100 -eq 0) { [void]$sb.Append("$e[?1049h") }
	[void]$sb.Append("$e[H$e[2J")
	for ($r = 1; $r -le 40; $r++) { [void]$sb.Append("$e[$r;1H$e[3$($r % 8)m$($row.Substring(0, 118))") }
	if ($f % 100 -eq 99) { [void]$sb.Append("$e[?1049l") }
}
$sb.ToString()
