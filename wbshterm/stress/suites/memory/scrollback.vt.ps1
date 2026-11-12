# Scrollback is capped (10 000 lines): ten times more output must not need much more memory.
# @stress size=120x40 tolerance=8 ratio=3
param([int]$Scale = 1)
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 50000 * $Scale; $i++) { [void]$sb.Append("scrollback line ${i} with some text after it`r`n") }
$sb.ToString()
