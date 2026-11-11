# Very long lines that wrap many times (minified files, logs without breaks).
# @stress size=120x40
param([int]$Scale = 1)
$chunk = 'abcdefghijklmnopqrstuvwxyz0123456789' * 500
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 400 * $Scale; $i++) { [void]$sb.Append("$i$chunk`r`n") }
$sb.ToString()
