# OSC 133 prompt / command marks from the shell: the command blocks are capped at 500.
# @stress size=120x40 ratio=3
param([int]$Scale = 1)
$e = [char]27; $bel = [char]7
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 20000 * $Scale; $i++) {
	[void]$sb.Append("$e]133;A$bel`$ $e]133;B$bel" + "echo $i`r`n$e]133;C$bel" + "out $i`r`n$e]133;D;0$bel")
}
$sb.ToString()
