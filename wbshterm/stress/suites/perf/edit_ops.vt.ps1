# Insert / delete characters and lines and erases, as line editors and vi send them.
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$ops = @("$e[3@", "$e[2P", "$e[L", "$e[M", "$e[K", "$e[1K", "$e[4X", "$e[J")
$rng = [System.Random]::new(2)
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 150000 * $Scale; $i++) { [void]$sb.Append("$e[$($rng.Next(1, 41));$($rng.Next(1, 121))H$($ops[$i % $ops.Count])text") }
$sb.ToString()
