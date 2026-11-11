# Entering and leaving the alternate screen with content each time (pagers, editors).
# 12 000 lines first fill the scrollback to its cap, so both scales start full.
# @stress size=120x40 ratio=3
param([int]$Scale = 1)
$e = [char]27
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 12000; $i++) { [void]$sb.Append("fill $i`r`n") }
for ($i = 0; $i -lt 5000 * $Scale; $i++) { [void]$sb.Append("$e[?1049h$e[2Jalt $i$e[20;1Hmore text$e[?1049lmain $i`r`n") }
$sb.ToString()
