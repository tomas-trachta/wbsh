# A program that sets the window title on every line (progress output, shells).
# @stress size=120x40 ratio=3
param([int]$Scale = 1)
$e = [char]27; $bel = [char]7
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 50000 * $Scale; $i++) { [void]$sb.Append("$e]0;building target $i of many$bel$e]2;step $i$bel.") }
$sb.ToString()
