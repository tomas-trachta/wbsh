# Plain ASCII lines: the floor cost of parsing, printing and scrolling.
# @stress size=120x40
param([int]$Scale = 1)
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 100000 * $Scale; $i++) { [void]$sb.Append("line ${i}: the quick brown fox jumps over the lazy dog`r`n") }
$sb.ToString()
