# Many distinct OSC 8 hyperlinks: the link table is capped at 65 535 entries.
# @stress size=120x40 ratio=3
param([int]$Scale = 1)
$e = [char]27; $bel = [char]7
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 20000 * $Scale; $i++) { [void]$sb.Append("$e]8;;https://example.com/page/$i$bel" + "link $i$e]8;;$bel`r`n") }
$sb.ToString()
