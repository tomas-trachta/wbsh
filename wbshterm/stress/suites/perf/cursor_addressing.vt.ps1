# Full-screen programs: random cursor moves with short writes (top, vi, htop).
# @stress size=120x40
param([int]$Scale = 1)
$e = [char]27
$rng = [System.Random]::new(1)
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 200000 * $Scale; $i++) { [void]$sb.Append("$e[$($rng.Next(1, 41));$($rng.Next(1, 121))Hx$($i % 10)") }
$sb.ToString()
