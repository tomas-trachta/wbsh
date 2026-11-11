# Every word in its own 256-colour SGR, the way ls --color and compilers print.
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 30000 * $Scale; $i++) {
	for ($w = 0; $w -lt 12; $w++) { [void]$sb.Append("$e[38;5;$(($i + $w) % 256)m$e[1mword$w$e[0m ") }
	[void]$sb.Append("`r`n")
}
$sb.ToString()
