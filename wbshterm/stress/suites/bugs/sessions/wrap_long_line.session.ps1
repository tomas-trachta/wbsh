# A 300-column line wraps over three rows of a 100-column grid.
param([string]$WorkDir, [int]$Scale = 1)
@{ Feed = @('printf "%0300d\n" 0\r'); Size = '100x30'; Expect = @('(?m)^0{100}\s*\r?\n0{100}\s*\r?\n0{100}\s*$') }
