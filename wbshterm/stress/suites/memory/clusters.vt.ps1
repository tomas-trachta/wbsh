# Many distinct grapheme clusters (base + combining marks): the cluster table is capped.
# The text is rewritten in place (cursor home), so the scrollback stays empty.
# @stress size=120x40 ratio=3
param([int]$Scale = 1)
$marks = 0x300..0x36F | ForEach-Object { [string][char]$_ }
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 20000 * $Scale; $i++) {
	$base = [char](0x41 + ($i % 26))
	$second = [Math]::Floor($i / 112) % 112
	$third = [Math]::Floor($i / 12544) % 112
	[void]$sb.Append("$base$($marks[$i % 112])$($marks[$second])$($marks[$third]) ")
	if ($i % 30 -eq 29) { [void]$sb.Append("$([char]27)[H") }
}
$sb.ToString()
