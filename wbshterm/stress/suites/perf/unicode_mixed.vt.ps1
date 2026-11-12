# Wide CJK, emoji (with ZWJ joins), combining marks and RTL letters on every line.
# @stress size=120x40
param([int]$Scale = 1)
$cjk = "$([char]0x4F60)$([char]0x597D)$([char]0x4E16)$([char]0x754C)"
$emoji = [char]::ConvertFromUtf32(0x1F600) + [char]::ConvertFromUtf32(0x1F468) + [char]0x200D + [char]::ConvertFromUtf32(0x1F469)
$comb = "e$([char]0x301)a$([char]0x308)o$([char]0x302)$([char]0x323)"
$rtl = "$([char]0x05E9)$([char]0x05DC)$([char]0x05D5)$([char]0x05DD)"
$line = "$cjk $emoji $comb $rtl ascii text " * 3
$sb = [System.Text.StringBuilder]::new()
for ($i = 0; $i -lt 20000 * $Scale; $i++) { [void]$sb.Append("$i $line`r`n") }
$sb.ToString()
