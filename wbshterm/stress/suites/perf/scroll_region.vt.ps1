# Scrolling inside a margin (DECSTBM), as pagers and status-line programs do.
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$sb = [System.Text.StringBuilder]::new()
[void]$sb.Append("$e[5;35r$e[35;1H")
for ($i = 0; $i -lt 100000 * $Scale; $i++) { [void]$sb.Append("region line $i`r`n") }
[void]$sb.Append("$e[r")
$sb.ToString()
