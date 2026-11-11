# Wide and combining characters typed and printed through the pseudoconsole.
param([string]$WorkDir, [int]$Scale = 1)
$wide = "$([char]0x4F60)$([char]0x597D)"
$comb = "e$([char]0x301)"
@{ Feed = @("echo `"[$wide|$comb]`"\r"); Expect = @("\[$wide\|$comb\]") }
